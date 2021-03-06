#define	JEMALLOC_ARENA_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

ssize_t		opt_lg_dirty_mult = LG_DIRTY_MULT_DEFAULT;
arena_bin_info_t	arena_bin_info[NBINS];

JEMALLOC_ALIGNED(CACHELINE)
const uint8_t	small_size2bin[] = {
#define	S2B_8(i)	i,
#define	S2B_16(i)	S2B_8(i) S2B_8(i)
#define	S2B_32(i)	S2B_16(i) S2B_16(i)
#define	S2B_64(i)	S2B_32(i) S2B_32(i)
#define	S2B_128(i)	S2B_64(i) S2B_64(i)
#define	S2B_256(i)	S2B_128(i) S2B_128(i)
#define	S2B_512(i)	S2B_256(i) S2B_256(i)
#define	S2B_1024(i)	S2B_512(i) S2B_512(i)
#define	S2B_2048(i)	S2B_1024(i) S2B_1024(i)
#define	S2B_4096(i)	S2B_2048(i) S2B_2048(i)
#define	S2B_8192(i)	S2B_4096(i) S2B_4096(i)
#define	SIZE_CLASS(bin, delta, size)					\
	S2B_##delta(bin)
	SIZE_CLASSES
#undef S2B_8
#undef S2B_16
#undef S2B_32
#undef S2B_64
#undef S2B_128
#undef S2B_256
#undef S2B_512
#undef S2B_1024
#undef S2B_2048
#undef S2B_4096
#undef S2B_8192
#undef SIZE_CLASS
};

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static void	arena_run_split(arena_t *arena, arena_run_t *run, size_t size,
    bool large, bool zero);
static arena_chunk_t *arena_chunk_alloc(arena_t *arena);
static void	arena_chunk_dealloc(arena_t *arena, arena_chunk_t *chunk);
static arena_run_t *arena_run_alloc(arena_t *arena, size_t size, bool large,
    bool zero);
static void	arena_purge(arena_t *arena, bool all);
static void	arena_run_dalloc(arena_t *arena, arena_run_t *run, bool dirty);
static void	arena_run_trim_head(arena_t *arena, arena_chunk_t *chunk,
    arena_run_t *run, size_t oldsize, size_t newsize);
static void	arena_run_trim_tail(arena_t *arena, arena_chunk_t *chunk,
    arena_run_t *run, size_t oldsize, size_t newsize, bool dirty);
static arena_run_t	*arena_bin_runs_first(arena_bin_t *bin);
static void	arena_bin_runs_insert(arena_bin_t *bin, arena_run_t *run);
static void	arena_bin_runs_remove(arena_bin_t *bin, arena_run_t *run);
static arena_run_t *arena_bin_nonfull_run_tryget(arena_bin_t *bin);
static arena_run_t *arena_bin_nonfull_run_get(arena_t *arena, arena_bin_t *bin);
static void	*arena_bin_malloc_hard(arena_t *arena, arena_bin_t *bin);
static void	arena_dissociate_bin_run(arena_chunk_t *chunk, arena_run_t *run,
    arena_bin_t *bin);
static void	arena_dalloc_bin_run(arena_t *arena, arena_chunk_t *chunk,
    arena_run_t *run, arena_bin_t *bin);
static void	arena_bin_lower_run(arena_t *arena, arena_chunk_t *chunk,
    arena_run_t *run, arena_bin_t *bin);
static void	arena_ralloc_large_shrink(arena_t *arena, arena_chunk_t *chunk,
    void *ptr, size_t oldsize, size_t size);
static bool	arena_ralloc_large_grow(arena_t *arena, arena_chunk_t *chunk,
    void *ptr, size_t oldsize, size_t size, size_t extra, bool zero);
static bool	arena_ralloc_large(void *ptr, size_t oldsize, size_t size,
    size_t extra, bool zero);
static size_t	bin_info_run_size_calc(arena_bin_info_t *bin_info,
    size_t min_run_size);
static void	bin_info_init(void);

/******************************************************************************/

static inline int
arena_run_comp(arena_chunk_map_t *a, arena_chunk_map_t *b)
{
	uintptr_t a_mapelm = (uintptr_t)a;
	uintptr_t b_mapelm = (uintptr_t)b;

	assert(a != NULL);
	assert(b != NULL);

	return ((a_mapelm > b_mapelm) - (a_mapelm < b_mapelm));
}

/* Generate red-black tree functions. */
rb_gen(static UNUSED, arena_run_tree_, arena_run_tree_t, arena_chunk_map_t,
    u.rb_link, arena_run_comp)

static inline int
arena_avail_comp(arena_chunk_map_t *a, arena_chunk_map_t *b)
{
	int ret;
	size_t a_size = a->bits & ~PAGE_MASK;
	size_t b_size = b->bits & ~PAGE_MASK;

	assert((a->bits & CHUNK_MAP_KEY) == CHUNK_MAP_KEY || (a->bits &
	    CHUNK_MAP_DIRTY) == (b->bits & CHUNK_MAP_DIRTY));

	ret = (a_size > b_size) - (a_size < b_size);
	if (ret == 0) {
		uintptr_t a_mapelm, b_mapelm;

		if ((a->bits & CHUNK_MAP_KEY) != CHUNK_MAP_KEY)
			a_mapelm = (uintptr_t)a;
		else {
			/*
			 * Treat keys as though they are lower than anything
			 * else.
			 */
			a_mapelm = 0;
		}
		b_mapelm = (uintptr_t)b;

		ret = (a_mapelm > b_mapelm) - (a_mapelm < b_mapelm);
	}

	return (ret);
}

/* Generate red-black tree functions. */
rb_gen(static UNUSED, arena_avail_tree_, arena_avail_tree_t, arena_chunk_map_t,
    u.rb_link, arena_avail_comp)

static inline void *
arena_run_reg_alloc(arena_run_t *run, arena_bin_info_t *bin_info)
{
	void *ret;
	unsigned regind;
	bitmap_t *bitmap = (bitmap_t *)((uintptr_t)run +
	    (uintptr_t)bin_info->bitmap_offset);

	assert(run->nfree > 0);
	assert(bitmap_full(bitmap, &bin_info->bitmap_info) == false);

	regind = bitmap_sfu(bitmap, &bin_info->bitmap_info);
	ret = (void *)((uintptr_t)run + (uintptr_t)bin_info->reg0_offset +
	    (uintptr_t)(bin_info->reg_interval * regind));
	run->nfree--;
	if (regind == run->nextind)
		run->nextind++;
	assert(regind < run->nextind);
	return (ret);
}

static inline void
arena_run_reg_dalloc(arena_run_t *run, void *ptr)
{
	arena_chunk_t *chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
	size_t binind = arena_bin_index(chunk->arena, run->bin);
	arena_bin_info_t *bin_info = &arena_bin_info[binind];
	unsigned regind = arena_run_regind(run, bin_info, ptr);
	bitmap_t *bitmap = (bitmap_t *)((uintptr_t)run +
	    (uintptr_t)bin_info->bitmap_offset);

	assert(run->nfree < bin_info->nregs);
	/* Freeing an interior pointer can cause assertion failure. */
	assert(((uintptr_t)ptr - ((uintptr_t)run +
	    (uintptr_t)bin_info->reg0_offset)) %
	    (uintptr_t)bin_info->reg_interval == 0);
	assert((uintptr_t)ptr >= (uintptr_t)run +
	    (uintptr_t)bin_info->reg0_offset);
	/* Freeing an unallocated pointer can cause assertion failure. */
	assert(bitmap_get(bitmap, &bin_info->bitmap_info, regind));

	bitmap_unset(bitmap, &bin_info->bitmap_info, regind);
	run->nfree++;
}

static inline void
arena_chunk_validate_zeroed(arena_chunk_t *chunk, size_t run_ind)
{
	size_t i;
	UNUSED size_t *p = (size_t *)((uintptr_t)chunk + (run_ind << LG_PAGE));

	for (i = 0; i < PAGE / sizeof(size_t); i++)
		assert(p[i] == 0);
}

static void
arena_run_split(arena_t *arena, arena_run_t *run, size_t size, bool large,
    bool zero)
{
	arena_chunk_t *chunk;
	size_t run_ind, total_pages, need_pages, rem_pages, i;
	size_t flag_dirty;
	arena_avail_tree_t *runs_avail;

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
	run_ind = (unsigned)(((uintptr_t)run - (uintptr_t)chunk) >> LG_PAGE);
	flag_dirty = chunk->map[run_ind-map_bias].bits & CHUNK_MAP_DIRTY;
	runs_avail = (flag_dirty != 0) ? &arena->runs_avail_dirty :
	    &arena->runs_avail_clean;
	total_pages = (chunk->map[run_ind-map_bias].bits & ~PAGE_MASK) >>
	    LG_PAGE;
	assert((chunk->map[run_ind+total_pages-1-map_bias].bits &
	    CHUNK_MAP_DIRTY) == flag_dirty);
	need_pages = (size >> LG_PAGE);
	assert(need_pages > 0);
	assert(need_pages <= total_pages);
	rem_pages = total_pages - need_pages;

	arena_avail_tree_remove(runs_avail, &chunk->map[run_ind-map_bias]);
	if (config_stats) {
		/*
		 * Update stats_cactive if nactive is crossing a chunk
		 * multiple.
		 */
		size_t cactive_diff = CHUNK_CEILING((arena->nactive +
		    need_pages) << LG_PAGE) - CHUNK_CEILING(arena->nactive <<
		    LG_PAGE);
		if (cactive_diff != 0)
			stats_cactive_add(cactive_diff);
	}
	arena->nactive += need_pages;

	/* Keep track of trailing unused pages for later use. */
	if (rem_pages > 0) {
		if (flag_dirty != 0) {
			chunk->map[run_ind+need_pages-map_bias].bits =
			    (rem_pages << LG_PAGE) | CHUNK_MAP_DIRTY;
			chunk->map[run_ind+total_pages-1-map_bias].bits =
			    (rem_pages << LG_PAGE) | CHUNK_MAP_DIRTY;
		} else {
			chunk->map[run_ind+need_pages-map_bias].bits =
			    (rem_pages << LG_PAGE) |
			    (chunk->map[run_ind+need_pages-map_bias].bits &
			    CHUNK_MAP_UNZEROED);
			chunk->map[run_ind+total_pages-1-map_bias].bits =
			    (rem_pages << LG_PAGE) |
			    (chunk->map[run_ind+total_pages-1-map_bias].bits &
			    CHUNK_MAP_UNZEROED);
		}
		arena_avail_tree_insert(runs_avail,
		    &chunk->map[run_ind+need_pages-map_bias]);
	}

	/* Update dirty page accounting. */
	if (flag_dirty != 0) {
		chunk->ndirty -= need_pages;
		arena->ndirty -= need_pages;
	}

	/*
	 * Update the page map separately for large vs. small runs, since it is
	 * possible to avoid iteration for large mallocs.
	 */
	if (large) {
		if (zero) {
			if (flag_dirty == 0) {
				/*
				 * The run is clean, so some pages may be
				 * zeroed (i.e. never before touched).
				 */
				for (i = 0; i < need_pages; i++) {
					if ((chunk->map[run_ind+i-map_bias].bits
					    & CHUNK_MAP_UNZEROED) != 0) {
						VALGRIND_MAKE_MEM_UNDEFINED(
						    (void *)((uintptr_t)
						    chunk + ((run_ind+i) <<
						    LG_PAGE)), PAGE);
						memset((void *)((uintptr_t)
						    chunk + ((run_ind+i) <<
						    LG_PAGE)), 0, PAGE);
					} else if (config_debug) {
						VALGRIND_MAKE_MEM_DEFINED(
						    (void *)((uintptr_t)
						    chunk + ((run_ind+i) <<
						    LG_PAGE)), PAGE);
						arena_chunk_validate_zeroed(
						    chunk, run_ind+i);
					}
				}
			} else {
				/*
				 * The run is dirty, so all pages must be
				 * zeroed.
				 */
				VALGRIND_MAKE_MEM_UNDEFINED((void
				    *)((uintptr_t)chunk + (run_ind <<
				    LG_PAGE)), (need_pages << LG_PAGE));
				memset((void *)((uintptr_t)chunk + (run_ind <<
				    LG_PAGE)), 0, (need_pages << LG_PAGE));
			}
		}

		/*
		 * Set the last element first, in case the run only contains one
		 * page (i.e. both statements set the same element).
		 */
		chunk->map[run_ind+need_pages-1-map_bias].bits =
		    CHUNK_MAP_LARGE | CHUNK_MAP_ALLOCATED | flag_dirty;
		chunk->map[run_ind-map_bias].bits = size | flag_dirty |
		    CHUNK_MAP_LARGE | CHUNK_MAP_ALLOCATED;
	} else {
		assert(zero == false);
		/*
		 * Propagate the dirty and unzeroed flags to the allocated
		 * small run, so that arena_dalloc_bin_run() has the ability to
		 * conditionally trim clean pages.
		 */
		chunk->map[run_ind-map_bias].bits =
		    (chunk->map[run_ind-map_bias].bits & CHUNK_MAP_UNZEROED) |
		    CHUNK_MAP_ALLOCATED | flag_dirty;
		/*
		 * The first page will always be dirtied during small run
		 * initialization, so a validation failure here would not
		 * actually cause an observable failure.
		 */
		if (config_debug && flag_dirty == 0 &&
		    (chunk->map[run_ind-map_bias].bits & CHUNK_MAP_UNZEROED)
		    == 0)
			arena_chunk_validate_zeroed(chunk, run_ind);
		for (i = 1; i < need_pages - 1; i++) {
			chunk->map[run_ind+i-map_bias].bits = (i << LG_PAGE)
			    | (chunk->map[run_ind+i-map_bias].bits &
			    CHUNK_MAP_UNZEROED) | CHUNK_MAP_ALLOCATED;
			if (config_debug && flag_dirty == 0 &&
			    (chunk->map[run_ind+i-map_bias].bits &
			    CHUNK_MAP_UNZEROED) == 0)
				arena_chunk_validate_zeroed(chunk, run_ind+i);
		}
		chunk->map[run_ind+need_pages-1-map_bias].bits = ((need_pages
		    - 1) << LG_PAGE) |
		    (chunk->map[run_ind+need_pages-1-map_bias].bits &
		    CHUNK_MAP_UNZEROED) | CHUNK_MAP_ALLOCATED | flag_dirty;
		if (config_debug && flag_dirty == 0 &&
		    (chunk->map[run_ind+need_pages-1-map_bias].bits &
		    CHUNK_MAP_UNZEROED) == 0) {
			arena_chunk_validate_zeroed(chunk,
			    run_ind+need_pages-1);
		}
	}
}

static arena_chunk_t *
arena_chunk_alloc(arena_t *arena)
{
	arena_chunk_t *chunk;
	size_t i;

	if (arena->spare != NULL) {
		arena_avail_tree_t *runs_avail;

		chunk = arena->spare;
		arena->spare = NULL;

		/* Insert the run into the appropriate runs_avail_* tree. */
		if ((chunk->map[0].bits & CHUNK_MAP_DIRTY) == 0)
			runs_avail = &arena->runs_avail_clean;
		else
			runs_avail = &arena->runs_avail_dirty;
		assert((chunk->map[0].bits & ~PAGE_MASK) == arena_maxclass);
		assert((chunk->map[chunk_npages-1-map_bias].bits & ~PAGE_MASK)
		    == arena_maxclass);
		assert((chunk->map[0].bits & CHUNK_MAP_DIRTY) ==
		    (chunk->map[chunk_npages-1-map_bias].bits &
		    CHUNK_MAP_DIRTY));
		arena_avail_tree_insert(runs_avail, &chunk->map[0]);
	} else {
		bool zero;
		size_t unzeroed;

		zero = false;
		malloc_mutex_unlock(&arena->lock);
		chunk = (arena_chunk_t *)chunk_alloc(chunksize, chunksize,
		    false, &zero);
		malloc_mutex_lock(&arena->lock);
		if (chunk == NULL)
			return (NULL);
		if (config_stats)
			arena->stats.mapped += chunksize;

		chunk->arena = arena;
		ql_elm_new(chunk, link_dirty);
		chunk->dirtied = false;

		/*
		 * Claim that no pages are in use, since the header is merely
		 * overhead.
		 */
		chunk->ndirty = 0;

		/*
		 * Initialize the map to contain one maximal free untouched run.
		 * Mark the pages as zeroed iff chunk_alloc() returned a zeroed
		 * chunk.
		 */
		unzeroed = zero ? 0 : CHUNK_MAP_UNZEROED;
		chunk->map[0].bits = arena_maxclass | unzeroed;
		/*
		 * There is no need to initialize the internal page map entries
		 * unless the chunk is not zeroed.
		 */
		if (zero == false) {
			for (i = map_bias+1; i < chunk_npages-1; i++)
				chunk->map[i-map_bias].bits = unzeroed;
		} else if (config_debug) {
			for (i = map_bias+1; i < chunk_npages-1; i++)
				assert(chunk->map[i-map_bias].bits == unzeroed);
		}
		chunk->map[chunk_npages-1-map_bias].bits = arena_maxclass |
		    unzeroed;

		/* Insert the run into the runs_avail_clean tree. */
		arena_avail_tree_insert(&arena->runs_avail_clean,
		    &chunk->map[0]);
	}

	return (chunk);
}

static void
arena_chunk_dealloc(arena_t *arena, arena_chunk_t *chunk)
{
	arena_avail_tree_t *runs_avail;

	/*
	 * Remove run from the appropriate runs_avail_* tree, so that the arena
	 * does not use it.
	 */
	if ((chunk->map[0].bits & CHUNK_MAP_DIRTY) == 0)
		runs_avail = &arena->runs_avail_clean;
	else
		runs_avail = &arena->runs_avail_dirty;
	arena_avail_tree_remove(runs_avail, &chunk->map[0]);

	if (arena->spare != NULL) {
		arena_chunk_t *spare = arena->spare;

		arena->spare = chunk;
		if (spare->dirtied) {
			ql_remove(&chunk->arena->chunks_dirty, spare,
			    link_dirty);
			arena->ndirty -= spare->ndirty;
		}
		malloc_mutex_unlock(&arena->lock);
		chunk_dealloc((void *)spare, chunksize, true);
		malloc_mutex_lock(&arena->lock);
		if (config_stats)
			arena->stats.mapped -= chunksize;
	} else
		arena->spare = chunk;
}

static arena_run_t *
arena_run_alloc(arena_t *arena, size_t size, bool large, bool zero)
{
	arena_chunk_t *chunk;
	arena_run_t *run;
	arena_chunk_map_t *mapelm, key;

	assert(size <= arena_maxclass);
	assert((size & PAGE_MASK) == 0);

	/* Search the arena's chunks for the lowest best fit. */
	key.bits = size | CHUNK_MAP_KEY;
	mapelm = arena_avail_tree_nsearch(&arena->runs_avail_dirty, &key);
	if (mapelm != NULL) {
		arena_chunk_t *run_chunk = CHUNK_ADDR2BASE(mapelm);
		size_t pageind = (((uintptr_t)mapelm -
		    (uintptr_t)run_chunk->map) / sizeof(arena_chunk_map_t))
		    + map_bias;

		run = (arena_run_t *)((uintptr_t)run_chunk + (pageind <<
		    LG_PAGE));
		arena_run_split(arena, run, size, large, zero);
		return (run);
	}
	mapelm = arena_avail_tree_nsearch(&arena->runs_avail_clean, &key);
	if (mapelm != NULL) {
		arena_chunk_t *run_chunk = CHUNK_ADDR2BASE(mapelm);
		size_t pageind = (((uintptr_t)mapelm -
		    (uintptr_t)run_chunk->map) / sizeof(arena_chunk_map_t))
		    + map_bias;

		run = (arena_run_t *)((uintptr_t)run_chunk + (pageind <<
		    LG_PAGE));
		arena_run_split(arena, run, size, large, zero);
		return (run);
	}

	/*
	 * No usable runs.  Create a new chunk from which to allocate the run.
	 */
	chunk = arena_chunk_alloc(arena);
	if (chunk != NULL) {
		run = (arena_run_t *)((uintptr_t)chunk + (map_bias << LG_PAGE));
		arena_run_split(arena, run, size, large, zero);
		return (run);
	}

	/*
	 * arena_chunk_alloc() failed, but another thread may have made
	 * sufficient memory available while this one dropped arena->lock in
	 * arena_chunk_alloc(), so search one more time.
	 */
	mapelm = arena_avail_tree_nsearch(&arena->runs_avail_dirty, &key);
	if (mapelm != NULL) {
		arena_chunk_t *run_chunk = CHUNK_ADDR2BASE(mapelm);
		size_t pageind = (((uintptr_t)mapelm -
		    (uintptr_t)run_chunk->map) / sizeof(arena_chunk_map_t))
		    + map_bias;

		run = (arena_run_t *)((uintptr_t)run_chunk + (pageind <<
		    LG_PAGE));
		arena_run_split(arena, run, size, large, zero);
		return (run);
	}
	mapelm = arena_avail_tree_nsearch(&arena->runs_avail_clean, &key);
	if (mapelm != NULL) {
		arena_chunk_t *run_chunk = CHUNK_ADDR2BASE(mapelm);
		size_t pageind = (((uintptr_t)mapelm -
		    (uintptr_t)run_chunk->map) / sizeof(arena_chunk_map_t))
		    + map_bias;

		run = (arena_run_t *)((uintptr_t)run_chunk + (pageind <<
		    LG_PAGE));
		arena_run_split(arena, run, size, large, zero);
		return (run);
	}

	return (NULL);
}

static inline void
arena_maybe_purge(arena_t *arena)
{

	/* Enforce opt_lg_dirty_mult. */
	if (opt_lg_dirty_mult >= 0 && arena->ndirty > arena->npurgatory &&
	    (arena->ndirty - arena->npurgatory) > chunk_npages &&
	    (arena->nactive >> opt_lg_dirty_mult) < (arena->ndirty -
	    arena->npurgatory))
		arena_purge(arena, false);
}

static inline void
arena_chunk_purge(arena_t *arena, arena_chunk_t *chunk)
{
	ql_head(arena_chunk_map_t) mapelms;
	arena_chunk_map_t *mapelm;
	size_t pageind, flag_unzeroed;
	size_t ndirty;
	size_t nmadvise;

	ql_new(&mapelms);

	flag_unzeroed =
#ifdef JEMALLOC_PURGE_MADVISE_DONTNEED
   /*
    * madvise(..., MADV_DONTNEED) results in zero-filled pages for anonymous
    * mappings, but not for file-backed mappings.
    */
	    0
#else
	    CHUNK_MAP_UNZEROED
#endif
	    ;

	/*
	 * If chunk is the spare, temporarily re-allocate it, 1) so that its
	 * run is reinserted into runs_avail_dirty, and 2) so that it cannot be
	 * completely discarded by another thread while arena->lock is dropped
	 * by this thread.  Note that the arena_run_dalloc() call will
	 * implicitly deallocate the chunk, so no explicit action is required
	 * in this function to deallocate the chunk.
	 *
	 * Note that once a chunk contains dirty pages, it cannot again contain
	 * a single run unless 1) it is a dirty run, or 2) this function purges
	 * dirty pages and causes the transition to a single clean run.  Thus
	 * (chunk == arena->spare) is possible, but it is not possible for
	 * this function to be called on the spare unless it contains a dirty
	 * run.
	 */
	if (chunk == arena->spare) {
		assert((chunk->map[0].bits & CHUNK_MAP_DIRTY) != 0);
		arena_chunk_alloc(arena);
	}

	/* Temporarily allocate all free dirty runs within chunk. */
	for (pageind = map_bias; pageind < chunk_npages;) {
		mapelm = &chunk->map[pageind-map_bias];
		if ((mapelm->bits & CHUNK_MAP_ALLOCATED) == 0) {
			size_t npages;

			npages = mapelm->bits >> LG_PAGE;
			assert(pageind + npages <= chunk_npages);
			if (mapelm->bits & CHUNK_MAP_DIRTY) {
				size_t i;

				arena_avail_tree_remove(
				    &arena->runs_avail_dirty, mapelm);

				mapelm->bits = (npages << LG_PAGE) |
				    flag_unzeroed | CHUNK_MAP_LARGE |
				    CHUNK_MAP_ALLOCATED;
				/*
				 * Update internal elements in the page map, so
				 * that CHUNK_MAP_UNZEROED is properly set.
				 */
				for (i = 1; i < npages - 1; i++) {
					chunk->map[pageind+i-map_bias].bits =
					    flag_unzeroed;
				}
				if (npages > 1) {
					chunk->map[
					    pageind+npages-1-map_bias].bits =
					    flag_unzeroed | CHUNK_MAP_LARGE |
					    CHUNK_MAP_ALLOCATED;
				}

				if (config_stats) {
					/*
					 * Update stats_cactive if nactive is
					 * crossing a chunk multiple.
					 */
					size_t cactive_diff =
					    CHUNK_CEILING((arena->nactive +
					    npages) << LG_PAGE) -
					    CHUNK_CEILING(arena->nactive <<
					    LG_PAGE);
					if (cactive_diff != 0)
						stats_cactive_add(cactive_diff);
				}
				arena->nactive += npages;
				/* Append to list for later processing. */
				ql_elm_new(mapelm, u.ql_link);
				ql_tail_insert(&mapelms, mapelm, u.ql_link);
			}

			pageind += npages;
		} else {
			/* Skip allocated run. */
			if (mapelm->bits & CHUNK_MAP_LARGE)
				pageind += mapelm->bits >> LG_PAGE;
			else {
				size_t binind;
				arena_bin_info_t *bin_info;
				arena_run_t *run = (arena_run_t *)((uintptr_t)
				    chunk + (uintptr_t)(pageind << LG_PAGE));

				assert((mapelm->bits >> LG_PAGE) == 0);
				binind = arena_bin_index(arena, run->bin);
				bin_info = &arena_bin_info[binind];
				pageind += bin_info->run_size >> LG_PAGE;
			}
		}
	}
	assert(pageind == chunk_npages);

	if (config_debug)
		ndirty = chunk->ndirty;
	if (config_stats)
		arena->stats.purged += chunk->ndirty;
	arena->ndirty -= chunk->ndirty;
	chunk->ndirty = 0;
	ql_remove(&arena->chunks_dirty, chunk, link_dirty);
	chunk->dirtied = false;

	malloc_mutex_unlock(&arena->lock);
	if (config_stats)
		nmadvise = 0;
	ql_foreach(mapelm, &mapelms, u.ql_link) {
		size_t pageind = (((uintptr_t)mapelm - (uintptr_t)chunk->map) /
		    sizeof(arena_chunk_map_t)) + map_bias;
		size_t npages = mapelm->bits >> LG_PAGE;

		assert(pageind + npages <= chunk_npages);
		assert(ndirty >= npages);
		if (config_debug)
			ndirty -= npages;

		pages_purge((void *)((uintptr_t)chunk + (pageind << LG_PAGE)),
		    (npages << LG_PAGE));
		if (config_stats)
			nmadvise++;
	}
	assert(ndirty == 0);
	malloc_mutex_lock(&arena->lock);
	if (config_stats)
		arena->stats.nmadvise += nmadvise;

	/* Deallocate runs. */
	for (mapelm = ql_first(&mapelms); mapelm != NULL;
	    mapelm = ql_first(&mapelms)) {
		size_t pageind = (((uintptr_t)mapelm - (uintptr_t)chunk->map) /
		    sizeof(arena_chunk_map_t)) + map_bias;
		arena_run_t *run = (arena_run_t *)((uintptr_t)chunk +
		    (uintptr_t)(pageind << LG_PAGE));

		ql_remove(&mapelms, mapelm, u.ql_link);
		arena_run_dalloc(arena, run, false);
	}
}

static void
arena_purge(arena_t *arena, bool all)
{
	arena_chunk_t *chunk;
	size_t npurgatory;
	if (config_debug) {
		size_t ndirty = 0;

		ql_foreach(chunk, &arena->chunks_dirty, link_dirty) {
		    assert(chunk->dirtied);
		    ndirty += chunk->ndirty;
		}
		assert(ndirty == arena->ndirty);
	}
	assert(arena->ndirty > arena->npurgatory || all);
	assert(arena->ndirty - arena->npurgatory > chunk_npages || all);
	assert((arena->nactive >> opt_lg_dirty_mult) < (arena->ndirty -
	    arena->npurgatory) || all);

	if (config_stats)
		arena->stats.npurge++;

	/*
	 * Compute the minimum number of pages that this thread should try to
	 * purge, and add the result to arena->npurgatory.  This will keep
	 * multiple threads from racing to reduce ndirty below the threshold.
	 */
	npurgatory = arena->ndirty - arena->npurgatory;
	if (all == false) {
		assert(npurgatory >= arena->nactive >> opt_lg_dirty_mult);
		npurgatory -= arena->nactive >> opt_lg_dirty_mult;
	}
	arena->npurgatory += npurgatory;

	while (npurgatory > 0) {
		/* Get next chunk with dirty pages. */
		chunk = ql_first(&arena->chunks_dirty);
		if (chunk == NULL) {
			/*
			 * This thread was unable to purge as many pages as
			 * originally intended, due to races with other threads
			 * that either did some of the purging work, or re-used
			 * dirty pages.
			 */
			arena->npurgatory -= npurgatory;
			return;
		}
		while (chunk->ndirty == 0) {
			ql_remove(&arena->chunks_dirty, chunk, link_dirty);
			chunk->dirtied = false;
			chunk = ql_first(&arena->chunks_dirty);
			if (chunk == NULL) {
				/* Same logic as for above. */
				arena->npurgatory -= npurgatory;
				return;
			}
		}

		if (chunk->ndirty > npurgatory) {
			/*
			 * This thread will, at a minimum, purge all the dirty
			 * pages in chunk, so set npurgatory to reflect this
			 * thread's commitment to purge the pages.  This tends
			 * to reduce the chances of the following scenario:
			 *
			 * 1) This thread sets arena->npurgatory such that
			 *    (arena->ndirty - arena->npurgatory) is at the
			 *    threshold.
			 * 2) This thread drops arena->lock.
			 * 3) Another thread causes one or more pages to be
			 *    dirtied, and immediately determines that it must
			 *    purge dirty pages.
			 *
			 * If this scenario *does* play out, that's okay,
			 * because all of the purging work being done really
			 * needs to happen.
			 */
			arena->npurgatory += chunk->ndirty - npurgatory;
			npurgatory = chunk->ndirty;
		}

		arena->npurgatory -= chunk->ndirty;
		npurgatory -= chunk->ndirty;
		arena_chunk_purge(arena, chunk);
	}
}

void
arena_purge_all(arena_t *arena)
{

	malloc_mutex_lock(&arena->lock);
	arena_purge(arena, true);
	malloc_mutex_unlock(&arena->lock);
}

static void
arena_run_dalloc(arena_t *arena, arena_run_t *run, bool dirty)
{
	arena_chunk_t *chunk;
	size_t size, run_ind, run_pages, flag_dirty;
	arena_avail_tree_t *runs_avail;

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
	run_ind = (size_t)(((uintptr_t)run - (uintptr_t)chunk) >> LG_PAGE);
	assert(run_ind >= map_bias);
	assert(run_ind < chunk_npages);
	if ((chunk->map[run_ind-map_bias].bits & CHUNK_MAP_LARGE) != 0) {
		size = chunk->map[run_ind-map_bias].bits & ~PAGE_MASK;
		assert(size == PAGE ||
		    (chunk->map[run_ind+(size>>LG_PAGE)-1-map_bias].bits &
		    ~PAGE_MASK) == 0);
		assert((chunk->map[run_ind+(size>>LG_PAGE)-1-map_bias].bits &
		    CHUNK_MAP_LARGE) != 0);
		assert((chunk->map[run_ind+(size>>LG_PAGE)-1-map_bias].bits &
		    CHUNK_MAP_ALLOCATED) != 0);
	} else {
		size_t binind = arena_bin_index(arena, run->bin);
		arena_bin_info_t *bin_info = &arena_bin_info[binind];
		size = bin_info->run_size;
	}
	run_pages = (size >> LG_PAGE);
	if (config_stats) {
		/*
		 * Update stats_cactive if nactive is crossing a chunk
		 * multiple.
		 */
		size_t cactive_diff = CHUNK_CEILING(arena->nactive << LG_PAGE) -
		    CHUNK_CEILING((arena->nactive - run_pages) << LG_PAGE);
		if (cactive_diff != 0)
			stats_cactive_sub(cactive_diff);
	}
	arena->nactive -= run_pages;

	/*
	 * The run is dirty if the caller claims to have dirtied it, as well as
	 * if it was already dirty before being allocated.
	 */
	if ((chunk->map[run_ind-map_bias].bits & CHUNK_MAP_DIRTY) != 0)
		dirty = true;
	flag_dirty = dirty ? CHUNK_MAP_DIRTY : 0;
	runs_avail = dirty ? &arena->runs_avail_dirty :
	    &arena->runs_avail_clean;

	/* Mark pages as unallocated in the chunk map. */
	if (dirty) {
		chunk->map[run_ind-map_bias].bits = size | CHUNK_MAP_DIRTY;
		chunk->map[run_ind+run_pages-1-map_bias].bits = size |
		    CHUNK_MAP_DIRTY;

		chunk->ndirty += run_pages;
		arena->ndirty += run_pages;
	} else {
		chunk->map[run_ind-map_bias].bits = size |
		    (chunk->map[run_ind-map_bias].bits & CHUNK_MAP_UNZEROED);
		chunk->map[run_ind+run_pages-1-map_bias].bits = size |
		    (chunk->map[run_ind+run_pages-1-map_bias].bits &
		    CHUNK_MAP_UNZEROED);
	}

	/* Try to coalesce forward. */
	if (run_ind + run_pages < chunk_npages &&
	    (chunk->map[run_ind+run_pages-map_bias].bits & CHUNK_MAP_ALLOCATED)
	    == 0 && (chunk->map[run_ind+run_pages-map_bias].bits &
	    CHUNK_MAP_DIRTY) == flag_dirty) {
		size_t nrun_size = chunk->map[run_ind+run_pages-map_bias].bits &
		    ~PAGE_MASK;
		size_t nrun_pages = nrun_size >> LG_PAGE;

		/*
		 * Remove successor from runs_avail; the coalesced run is
		 * inserted later.
		 */
		assert((chunk->map[run_ind+run_pages+nrun_pages-1-map_bias].bits
		    & ~PAGE_MASK) == nrun_size);
		assert((chunk->map[run_ind+run_pages+nrun_pages-1-map_bias].bits
		    & CHUNK_MAP_ALLOCATED) == 0);
		assert((chunk->map[run_ind+run_pages+nrun_pages-1-map_bias].bits
		    & CHUNK_MAP_DIRTY) == flag_dirty);
		arena_avail_tree_remove(runs_avail,
		    &chunk->map[run_ind+run_pages-map_bias]);

		size += nrun_size;
		run_pages += nrun_pages;

		chunk->map[run_ind-map_bias].bits = size |
		    (chunk->map[run_ind-map_bias].bits & CHUNK_MAP_FLAGS_MASK);
		chunk->map[run_ind+run_pages-1-map_bias].bits = size |
		    (chunk->map[run_ind+run_pages-1-map_bias].bits &
		    CHUNK_MAP_FLAGS_MASK);
	}

	/* Try to coalesce backward. */
	if (run_ind > map_bias && (chunk->map[run_ind-1-map_bias].bits &
	    CHUNK_MAP_ALLOCATED) == 0 && (chunk->map[run_ind-1-map_bias].bits &
	    CHUNK_MAP_DIRTY) == flag_dirty) {
		size_t prun_size = chunk->map[run_ind-1-map_bias].bits &
		    ~PAGE_MASK;
		size_t prun_pages = prun_size >> LG_PAGE;

		run_ind -= prun_pages;

		/*
		 * Remove predecessor from runs_avail; the coalesced run is
		 * inserted later.
		 */
		assert((chunk->map[run_ind-map_bias].bits & ~PAGE_MASK)
		    == prun_size);
		assert((chunk->map[run_ind-map_bias].bits & CHUNK_MAP_ALLOCATED)
		    == 0);
		assert((chunk->map[run_ind-map_bias].bits & CHUNK_MAP_DIRTY)
		    == flag_dirty);
		arena_avail_tree_remove(runs_avail,
		    &chunk->map[run_ind-map_bias]);

		size += prun_size;
		run_pages += prun_pages;

		chunk->map[run_ind-map_bias].bits = size |
		    (chunk->map[run_ind-map_bias].bits & CHUNK_MAP_FLAGS_MASK);
		chunk->map[run_ind+run_pages-1-map_bias].bits = size |
		    (chunk->map[run_ind+run_pages-1-map_bias].bits &
		    CHUNK_MAP_FLAGS_MASK);
	}

	/* Insert into runs_avail, now that coalescing is complete. */
	assert((chunk->map[run_ind-map_bias].bits & ~PAGE_MASK) ==
	    (chunk->map[run_ind+run_pages-1-map_bias].bits & ~PAGE_MASK));
	assert((chunk->map[run_ind-map_bias].bits & CHUNK_MAP_DIRTY) ==
	    (chunk->map[run_ind+run_pages-1-map_bias].bits & CHUNK_MAP_DIRTY));
	arena_avail_tree_insert(runs_avail, &chunk->map[run_ind-map_bias]);

	if (dirty) {
		/*
		 * Insert into chunks_dirty before potentially calling
		 * arena_chunk_dealloc(), so that chunks_dirty and
		 * arena->ndirty are consistent.
		 */
		if (chunk->dirtied == false) {
			ql_tail_insert(&arena->chunks_dirty, chunk, link_dirty);
			chunk->dirtied = true;
		}
	}

	/*
	 * Deallocate chunk if it is now completely unused.  The bit
	 * manipulation checks whether the first run is unallocated and extends
	 * to the end of the chunk.
	 */
	if ((chunk->map[0].bits & (~PAGE_MASK | CHUNK_MAP_ALLOCATED)) ==
	    arena_maxclass)
		arena_chunk_dealloc(arena, chunk);

	/*
	 * It is okay to do dirty page processing here even if the chunk was
	 * deallocated above, since in that case it is the spare.  Waiting
	 * until after possible chunk deallocation to do dirty processing
	 * allows for an old spare to be fully deallocated, thus decreasing the
	 * chances of spuriously crossing the dirty page purging threshold.
	 */
	if (dirty)
		arena_maybe_purge(arena);
}

static void
arena_run_trim_head(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    size_t oldsize, size_t newsize)
{
	size_t pageind = ((uintptr_t)run - (uintptr_t)chunk) >> LG_PAGE;
	size_t head_npages = (oldsize - newsize) >> LG_PAGE;
	size_t flag_dirty = chunk->map[pageind-map_bias].bits & CHUNK_MAP_DIRTY;

	assert(oldsize > newsize);

	/*
	 * Update the chunk map so that arena_run_dalloc() can treat the
	 * leading run as separately allocated.  Set the last element of each
	 * run first, in case of single-page runs.
	 */
	assert((chunk->map[pageind-map_bias].bits & CHUNK_MAP_LARGE) != 0);
	assert((chunk->map[pageind-map_bias].bits & CHUNK_MAP_ALLOCATED) != 0);
	chunk->map[pageind+head_npages-1-map_bias].bits = flag_dirty |
	    (chunk->map[pageind+head_npages-1-map_bias].bits &
	    CHUNK_MAP_UNZEROED) | CHUNK_MAP_LARGE | CHUNK_MAP_ALLOCATED;
	chunk->map[pageind-map_bias].bits = (oldsize - newsize)
	    | flag_dirty | (chunk->map[pageind-map_bias].bits &
	    CHUNK_MAP_UNZEROED) | CHUNK_MAP_LARGE | CHUNK_MAP_ALLOCATED;

	if (config_debug) {
		UNUSED size_t tail_npages = newsize >> LG_PAGE;
		assert((chunk->map[pageind+head_npages+tail_npages-1-map_bias]
		    .bits & ~PAGE_MASK) == 0);
		assert((chunk->map[pageind+head_npages+tail_npages-1-map_bias]
		    .bits & CHUNK_MAP_DIRTY) == flag_dirty);
		assert((chunk->map[pageind+head_npages+tail_npages-1-map_bias]
		    .bits & CHUNK_MAP_LARGE) != 0);
		assert((chunk->map[pageind+head_npages+tail_npages-1-map_bias]
		    .bits & CHUNK_MAP_ALLOCATED) != 0);
	}
	chunk->map[pageind+head_npages-map_bias].bits = newsize | flag_dirty |
	    (chunk->map[pageind+head_npages-map_bias].bits &
	    CHUNK_MAP_FLAGS_MASK) | CHUNK_MAP_LARGE | CHUNK_MAP_ALLOCATED;

	arena_run_dalloc(arena, run, false);
}

static void
arena_run_trim_tail(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    size_t oldsize, size_t newsize, bool dirty)
{
	size_t pageind = ((uintptr_t)run - (uintptr_t)chunk) >> LG_PAGE;
	size_t head_npages = newsize >> LG_PAGE;
	size_t tail_npages = (oldsize - newsize) >> LG_PAGE;
	size_t flag_dirty = chunk->map[pageind-map_bias].bits &
	    CHUNK_MAP_DIRTY;

	assert(oldsize > newsize);

	/*
	 * Update the chunk map so that arena_run_dalloc() can treat the
	 * trailing run as separately allocated.  Set the last element of each
	 * run first, in case of single-page runs.
	 */
	assert((chunk->map[pageind-map_bias].bits & CHUNK_MAP_LARGE) != 0);
	assert((chunk->map[pageind-map_bias].bits & CHUNK_MAP_ALLOCATED) != 0);
	chunk->map[pageind+head_npages-1-map_bias].bits = flag_dirty |
	    (chunk->map[pageind+head_npages-1-map_bias].bits &
	    CHUNK_MAP_UNZEROED) | CHUNK_MAP_LARGE | CHUNK_MAP_ALLOCATED;
	chunk->map[pageind-map_bias].bits = newsize | flag_dirty |
	    (chunk->map[pageind-map_bias].bits & CHUNK_MAP_UNZEROED) |
	    CHUNK_MAP_LARGE | CHUNK_MAP_ALLOCATED;

	assert((chunk->map[pageind+head_npages+tail_npages-1-map_bias].bits &
	    ~PAGE_MASK) == 0);
	assert((chunk->map[pageind+head_npages+tail_npages-1-map_bias].bits &
	    CHUNK_MAP_LARGE) != 0);
	assert((chunk->map[pageind+head_npages+tail_npages-1-map_bias].bits &
	    CHUNK_MAP_ALLOCATED) != 0);
	chunk->map[pageind+head_npages+tail_npages-1-map_bias].bits =
	    flag_dirty |
	    (chunk->map[pageind+head_npages+tail_npages-1-map_bias].bits &
	    CHUNK_MAP_UNZEROED) | CHUNK_MAP_LARGE | CHUNK_MAP_ALLOCATED;
	chunk->map[pageind+head_npages-map_bias].bits = (oldsize - newsize) |
	    flag_dirty | (chunk->map[pageind+head_npages-map_bias].bits &
	    CHUNK_MAP_UNZEROED) | CHUNK_MAP_LARGE | CHUNK_MAP_ALLOCATED;

	arena_run_dalloc(arena, (arena_run_t *)((uintptr_t)run + newsize),
	    dirty);
}

static arena_run_t *
arena_bin_runs_first(arena_bin_t *bin)
{
	arena_chunk_map_t *mapelm = arena_run_tree_first(&bin->runs);
	if (mapelm != NULL) {
		arena_chunk_t *chunk;
		size_t pageind;
		arena_run_t *run;

		chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(mapelm);
		pageind = ((((uintptr_t)mapelm - (uintptr_t)chunk->map) /
		    sizeof(arena_chunk_map_t))) + map_bias;
		run = (arena_run_t *)((uintptr_t)chunk +
		    (uintptr_t)((pageind - (mapelm->bits >> LG_PAGE)) <<
		    LG_PAGE));
		return (run);
	}

	return (NULL);
}

static void
arena_bin_runs_insert(arena_bin_t *bin, arena_run_t *run)
{
	arena_chunk_t *chunk = CHUNK_ADDR2BASE(run);
	size_t pageind = ((uintptr_t)run - (uintptr_t)chunk) >> LG_PAGE;
	arena_chunk_map_t *mapelm = &chunk->map[pageind-map_bias];

	assert(arena_run_tree_search(&bin->runs, mapelm) == NULL);

	arena_run_tree_insert(&bin->runs, mapelm);
}

static void
arena_bin_runs_remove(arena_bin_t *bin, arena_run_t *run)
{
	arena_chunk_t *chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
	size_t pageind = ((uintptr_t)run - (uintptr_t)chunk) >> LG_PAGE;
	arena_chunk_map_t *mapelm = &chunk->map[pageind-map_bias];

	assert(arena_run_tree_search(&bin->runs, mapelm) != NULL);

	arena_run_tree_remove(&bin->runs, mapelm);
}

static arena_run_t *
arena_bin_nonfull_run_tryget(arena_bin_t *bin)
{
	arena_run_t *run = arena_bin_runs_first(bin);
	if (run != NULL) {
		arena_bin_runs_remove(bin, run);
		if (config_stats)
			bin->stats.reruns++;
	}
	return (run);
}

static arena_run_t *
arena_bin_nonfull_run_get(arena_t *arena, arena_bin_t *bin)
{
	arena_run_t *run;
	size_t binind;
	arena_bin_info_t *bin_info;

	/* Look for a usable run. */
	run = arena_bin_nonfull_run_tryget(bin);
	if (run != NULL)
		return (run);
	/* No existing runs have any space available. */

	binind = arena_bin_index(arena, bin);
	bin_info = &arena_bin_info[binind];

	/* Allocate a new run. */
	malloc_mutex_unlock(&bin->lock);
	/******************************/
	malloc_mutex_lock(&arena->lock);
	run = arena_run_alloc(arena, bin_info->run_size, false, false);
	if (run != NULL) {
		bitmap_t *bitmap = (bitmap_t *)((uintptr_t)run +
		    (uintptr_t)bin_info->bitmap_offset);

		/* Initialize run internals. */
		VALGRIND_MAKE_MEM_UNDEFINED(run, bin_info->reg0_offset -
		    bin_info->redzone_size);
		run->bin = bin;
		run->nextind = 0;
		run->nfree = bin_info->nregs;
		bitmap_init(bitmap, &bin_info->bitmap_info);
	}
	malloc_mutex_unlock(&arena->lock);
	/********************************/
	malloc_mutex_lock(&bin->lock);
	if (run != NULL) {
		if (config_stats) {
			bin->stats.nruns++;
			bin->stats.curruns++;
		}
		return (run);
	}

	/*
	 * arena_run_alloc() failed, but another thread may have made
	 * sufficient memory available while this one dropped bin->lock above,
	 * so search one more time.
	 */
	run = arena_bin_nonfull_run_tryget(bin);
	if (run != NULL)
		return (run);

	return (NULL);
}

/* Re-fill bin->runcur, then call arena_run_reg_alloc(). */
static void *
arena_bin_malloc_hard(arena_t *arena, arena_bin_t *bin)
{
	void *ret;
	size_t binind;
	arena_bin_info_t *bin_info;
	arena_run_t *run;

	binind = arena_bin_index(arena, bin);
	bin_info = &arena_bin_info[binind];
	bin->runcur = NULL;
	run = arena_bin_nonfull_run_get(arena, bin);
	if (bin->runcur != NULL && bin->runcur->nfree > 0) {
		/*
		 * Another thread updated runcur while this one ran without the
		 * bin lock in arena_bin_nonfull_run_get().
		 */
		assert(bin->runcur->nfree > 0);
		ret = arena_run_reg_alloc(bin->runcur, bin_info);
		if (run != NULL) {
			arena_chunk_t *chunk;

			/*
			 * arena_run_alloc() may have allocated run, or it may
			 * have pulled run from the bin's run tree.  Therefore
			 * it is unsafe to make any assumptions about how run
			 * has previously been used, and arena_bin_lower_run()
			 * must be called, as if a region were just deallocated
			 * from the run.
			 */
			chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
			if (run->nfree == bin_info->nregs)
				arena_dalloc_bin_run(arena, chunk, run, bin);
			else
				arena_bin_lower_run(arena, chunk, run, bin);
		}
		return (ret);
	}

	if (run == NULL)
		return (NULL);

	bin->runcur = run;

	assert(bin->runcur->nfree > 0);

	return (arena_run_reg_alloc(bin->runcur, bin_info));
}

void
arena_prof_accum(arena_t *arena, uint64_t accumbytes)
{

	cassert(config_prof);

	if (config_prof && prof_interval != 0) {
		arena->prof_accumbytes += accumbytes;
		if (arena->prof_accumbytes >= prof_interval) {
			prof_idump();
			arena->prof_accumbytes -= prof_interval;
		}
	}
}

void
arena_tcache_fill_small(arena_t *arena, tcache_bin_t *tbin, size_t binind,
    uint64_t prof_accumbytes)
{
	unsigned i, nfill;
	arena_bin_t *bin;
	arena_run_t *run;
	void *ptr;

	assert(tbin->ncached == 0);

	if (config_prof) {
		malloc_mutex_lock(&arena->lock);
		arena_prof_accum(arena, prof_accumbytes);
		malloc_mutex_unlock(&arena->lock);
	}
	bin = &arena->bins[binind];
	malloc_mutex_lock(&bin->lock);
	for (i = 0, nfill = (tcache_bin_info[binind].ncached_max >>
	    tbin->lg_fill_div); i < nfill; i++) {
		if ((run = bin->runcur) != NULL && run->nfree > 0)
			ptr = arena_run_reg_alloc(run, &arena_bin_info[binind]);
		else
			ptr = arena_bin_malloc_hard(arena, bin);
		if (ptr == NULL)
			break;
		if (config_fill && opt_junk) {
			arena_alloc_junk_small(ptr, &arena_bin_info[binind],
			    true);
		}
		/* Insert such that low regions get used first. */
		tbin->avail[nfill - 1 - i] = ptr;
	}
	if (config_stats) {
		bin->stats.allocated += i * arena_bin_info[binind].reg_size;
		bin->stats.nmalloc += i;
		bin->stats.nrequests += tbin->tstats.nrequests;
		bin->stats.nfills++;
		tbin->tstats.nrequests = 0;
	}
	malloc_mutex_unlock(&bin->lock);
	tbin->ncached = i;
}

void
arena_alloc_junk_small(void *ptr, arena_bin_info_t *bin_info, bool zero)
{

	if (zero) {
		size_t redzone_size = bin_info->redzone_size;
		memset((void *)((uintptr_t)ptr - redzone_size), 0xa5,
		    redzone_size);
		memset((void *)((uintptr_t)ptr + bin_info->reg_size), 0xa5,
		    redzone_size);
	} else {
		memset((void *)((uintptr_t)ptr - bin_info->redzone_size), 0xa5,
		    bin_info->reg_interval);
	}
}

void
arena_dalloc_junk_small(void *ptr, arena_bin_info_t *bin_info)
{
	size_t size = bin_info->reg_size;
	size_t redzone_size = bin_info->redzone_size;
	size_t i;
	bool error = false;

	for (i = 1; i <= redzone_size; i++) {
		unsigned byte;
		if ((byte = *(uint8_t *)((uintptr_t)ptr - i)) != 0xa5) {
			error = true;
			malloc_printf("<jemalloc>: Corrupt redzone "
			    "%zu byte%s before %p (size %zu), byte=%#x\n", i,
			    (i == 1) ? "" : "s", ptr, size, byte);
		}
	}
	for (i = 0; i < redzone_size; i++) {
		unsigned byte;
		if ((byte = *(uint8_t *)((uintptr_t)ptr + size + i)) != 0xa5) {
			error = true;
			malloc_printf("<jemalloc>: Corrupt redzone "
			    "%zu byte%s after end of %p (size %zu), byte=%#x\n",
			    i, (i == 1) ? "" : "s", ptr, size, byte);
		}
	}
	if (opt_abort && error)
		abort();

	memset((void *)((uintptr_t)ptr - redzone_size), 0x5a,
	    bin_info->reg_interval);
}

void *
arena_malloc_small(arena_t *arena, size_t size, bool zero)
{
	void *ret;
	arena_bin_t *bin;
	arena_run_t *run;
	size_t binind;

	binind = SMALL_SIZE2BIN(size);
	assert(binind < NBINS);
	bin = &arena->bins[binind];
	size = arena_bin_info[binind].reg_size;

	malloc_mutex_lock(&bin->lock);
	if ((run = bin->runcur) != NULL && run->nfree > 0)
		ret = arena_run_reg_alloc(run, &arena_bin_info[binind]);
	else
		ret = arena_bin_malloc_hard(arena, bin);

	if (ret == NULL) {
		malloc_mutex_unlock(&bin->lock);
		return (NULL);
	}

	if (config_stats) {
		bin->stats.allocated += size;
		bin->stats.nmalloc++;
		bin->stats.nrequests++;
	}
	malloc_mutex_unlock(&bin->lock);
	if (config_prof && isthreaded == false) {
		malloc_mutex_lock(&arena->lock);
		arena_prof_accum(arena, size);
		malloc_mutex_unlock(&arena->lock);
	}

	if (zero == false) {
		if (config_fill) {
			if (opt_junk) {
				arena_alloc_junk_small(ret,
				    &arena_bin_info[binind], false);
			} else if (opt_zero)
				memset(ret, 0, size);
		}
	} else {
		if (config_fill && opt_junk) {
			arena_alloc_junk_small(ret, &arena_bin_info[binind],
			    true);
		}
		VALGRIND_MAKE_MEM_UNDEFINED(ret, size);
		memset(ret, 0, size);
	}

	return (ret);
}

void *
arena_malloc_large(arena_t *arena, size_t size, bool zero)
{
	void *ret;

	/* Large allocation. */
	size = PAGE_CEILING(size);
	malloc_mutex_lock(&arena->lock);
	ret = (void *)arena_run_alloc(arena, size, true, zero);
	if (ret == NULL) {
		malloc_mutex_unlock(&arena->lock);
		return (NULL);
	}
	if (config_stats) {
		arena->stats.nmalloc_large++;
		arena->stats.nrequests_large++;
		arena->stats.allocated_large += size;
		arena->stats.lstats[(size >> LG_PAGE) - 1].nmalloc++;
		arena->stats.lstats[(size >> LG_PAGE) - 1].nrequests++;
		arena->stats.lstats[(size >> LG_PAGE) - 1].curruns++;
	}
	if (config_prof)
		arena_prof_accum(arena, size);
	malloc_mutex_unlock(&arena->lock);

	if (zero == false) {
		if (config_fill) {
			if (opt_junk)
				memset(ret, 0xa5, size);
			else if (opt_zero)
				memset(ret, 0, size);
		}
	}

	return (ret);
}

/* Only handles large allocations that require more than page alignment. */
void *
arena_palloc(arena_t *arena, size_t size, size_t alignment, bool zero)
{
	void *ret;
	size_t alloc_size, leadsize, trailsize;
	arena_run_t *run;
	arena_chunk_t *chunk;

	assert((size & PAGE_MASK) == 0);

	alignment = PAGE_CEILING(alignment);
	alloc_size = size + alignment - PAGE;

	malloc_mutex_lock(&arena->lock);
	run = arena_run_alloc(arena, alloc_size, true, zero);
	if (run == NULL) {
		malloc_mutex_unlock(&arena->lock);
		return (NULL);
	}
	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);

	leadsize = ALIGNMENT_CEILING((uintptr_t)run, alignment) -
	    (uintptr_t)run;
	assert(alloc_size >= leadsize + size);
	trailsize = alloc_size - leadsize - size;
	ret = (void *)((uintptr_t)run + leadsize);
	if (leadsize != 0) {
		arena_run_trim_head(arena, chunk, run, alloc_size, alloc_size -
		    leadsize);
	}
	if (trailsize != 0) {
		arena_run_trim_tail(arena, chunk, ret, size + trailsize, size,
		    false);
	}

	if (config_stats) {
		arena->stats.nmalloc_large++;
		arena->stats.nrequests_large++;
		arena->stats.allocated_large += size;
		arena->stats.lstats[(size >> LG_PAGE) - 1].nmalloc++;
		arena->stats.lstats[(size >> LG_PAGE) - 1].nrequests++;
		arena->stats.lstats[(size >> LG_PAGE) - 1].curruns++;
	}
	malloc_mutex_unlock(&arena->lock);

	if (config_fill && zero == false) {
		if (opt_junk)
			memset(ret, 0xa5, size);
		else if (opt_zero)
			memset(ret, 0, size);
	}
	return (ret);
}

void
arena_prof_promoted(const void *ptr, size_t size)
{
	arena_chunk_t *chunk;
	size_t pageind, binind;

	cassert(config_prof);
	assert(ptr != NULL);
	assert(CHUNK_ADDR2BASE(ptr) != ptr);
	assert(isalloc(ptr, false) == PAGE);
	assert(isalloc(ptr, true) == PAGE);
	assert(size <= SMALL_MAXCLASS);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	binind = SMALL_SIZE2BIN(size);
	assert(binind < NBINS);
	chunk->map[pageind-map_bias].bits = (chunk->map[pageind-map_bias].bits &
	    ~CHUNK_MAP_CLASS_MASK) | ((binind+1) << CHUNK_MAP_CLASS_SHIFT);

	assert(isalloc(ptr, false) == PAGE);
	assert(isalloc(ptr, true) == size);
}

static void
arena_dissociate_bin_run(arena_chunk_t *chunk, arena_run_t *run,
    arena_bin_t *bin)
{

	/* Dissociate run from bin. */
	if (run == bin->runcur)
		bin->runcur = NULL;
	else {
		size_t binind = arena_bin_index(chunk->arena, bin);
		arena_bin_info_t *bin_info = &arena_bin_info[binind];

		if (bin_info->nregs != 1) {
			/*
			 * This block's conditional is necessary because if the
			 * run only contains one region, then it never gets
			 * inserted into the non-full runs tree.
			 */
			arena_bin_runs_remove(bin, run);
		}
	}
}

static void
arena_dalloc_bin_run(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    arena_bin_t *bin)
{
	size_t binind;
	arena_bin_info_t *bin_info;
	size_t npages, run_ind, past;

	assert(run != bin->runcur);
	assert(arena_run_tree_search(&bin->runs, &chunk->map[
	    (((uintptr_t)run-(uintptr_t)chunk)>>LG_PAGE)-map_bias]) == NULL);

	binind = arena_bin_index(chunk->arena, run->bin);
	bin_info = &arena_bin_info[binind];

	malloc_mutex_unlock(&bin->lock);
	/******************************/
	npages = bin_info->run_size >> LG_PAGE;
	run_ind = (size_t)(((uintptr_t)run - (uintptr_t)chunk) >> LG_PAGE);
	past = (size_t)(PAGE_CEILING((uintptr_t)run +
	    (uintptr_t)bin_info->reg0_offset + (uintptr_t)(run->nextind *
	    bin_info->reg_interval - bin_info->redzone_size) -
	    (uintptr_t)chunk) >> LG_PAGE);
	malloc_mutex_lock(&arena->lock);

	/*
	 * If the run was originally clean, and some pages were never touched,
	 * trim the clean pages before deallocating the dirty portion of the
	 * run.
	 */
	if ((chunk->map[run_ind-map_bias].bits & CHUNK_MAP_DIRTY) == 0 && past
	    - run_ind < npages) {
		/*
		 * Trim clean pages.  Convert to large run beforehand.  Set the
		 * last map element first, in case this is a one-page run.
		 */
		chunk->map[run_ind+npages-1-map_bias].bits = CHUNK_MAP_LARGE |
		    (chunk->map[run_ind+npages-1-map_bias].bits &
		    CHUNK_MAP_FLAGS_MASK);
		chunk->map[run_ind-map_bias].bits = bin_info->run_size |
		    CHUNK_MAP_LARGE | (chunk->map[run_ind-map_bias].bits &
		    CHUNK_MAP_FLAGS_MASK);
		arena_run_trim_tail(arena, chunk, run, (npages << LG_PAGE),
		    ((past - run_ind) << LG_PAGE), false);
		/* npages = past - run_ind; */
	}
	arena_run_dalloc(arena, run, true);
	malloc_mutex_unlock(&arena->lock);
	/****************************/
	malloc_mutex_lock(&bin->lock);
	if (config_stats)
		bin->stats.curruns--;
}

static void
arena_bin_lower_run(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    arena_bin_t *bin)
{

	/*
	 * Make sure that if bin->runcur is non-NULL, it refers to the lowest
	 * non-full run.  It is okay to NULL runcur out rather than proactively
	 * keeping it pointing at the lowest non-full run.
	 */
	if ((uintptr_t)run < (uintptr_t)bin->runcur) {
		/* Switch runcur. */
		if (bin->runcur->nfree > 0)
			arena_bin_runs_insert(bin, bin->runcur);
		bin->runcur = run;
		if (config_stats)
			bin->stats.reruns++;
	} else
		arena_bin_runs_insert(bin, run);
}

void
arena_dalloc_bin(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    arena_chunk_map_t *mapelm)
{
	size_t pageind;
	arena_run_t *run;
	arena_bin_t *bin;
	arena_bin_info_t *bin_info;
	size_t size, binind;

	pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	run = (arena_run_t *)((uintptr_t)chunk + (uintptr_t)((pageind -
	    (mapelm->bits >> LG_PAGE)) << LG_PAGE));
	bin = run->bin;
	binind = arena_bin_index(arena, bin);
	bin_info = &arena_bin_info[binind];
	if (config_fill || config_stats)
		size = bin_info->reg_size;

	if (config_fill && opt_junk)
		arena_dalloc_junk_small(ptr, bin_info);

	arena_run_reg_dalloc(run, ptr);
	if (run->nfree == bin_info->nregs) {
		arena_dissociate_bin_run(chunk, run, bin);
		arena_dalloc_bin_run(arena, chunk, run, bin);
	} else if (run->nfree == 1 && run != bin->runcur)
		arena_bin_lower_run(arena, chunk, run, bin);

	if (config_stats) {
		bin->stats.allocated -= size;
		bin->stats.ndalloc++;
	}
}

void
arena_stats_merge(arena_t *arena, size_t *nactive, size_t *ndirty,
    arena_stats_t *astats, malloc_bin_stats_t *bstats,
    malloc_large_stats_t *lstats)
{
	unsigned i;

	malloc_mutex_lock(&arena->lock);
	*nactive += arena->nactive;
	*ndirty += arena->ndirty;

	astats->mapped += arena->stats.mapped;
	astats->npurge += arena->stats.npurge;
	astats->nmadvise += arena->stats.nmadvise;
	astats->purged += arena->stats.purged;
	astats->allocated_large += arena->stats.allocated_large;
	astats->nmalloc_large += arena->stats.nmalloc_large;
	astats->ndalloc_large += arena->stats.ndalloc_large;
	astats->nrequests_large += arena->stats.nrequests_large;

	for (i = 0; i < nlclasses; i++) {
		lstats[i].nmalloc += arena->stats.lstats[i].nmalloc;
		lstats[i].ndalloc += arena->stats.lstats[i].ndalloc;
		lstats[i].nrequests += arena->stats.lstats[i].nrequests;
		lstats[i].curruns += arena->stats.lstats[i].curruns;
	}
	malloc_mutex_unlock(&arena->lock);

	for (i = 0; i < NBINS; i++) {
		arena_bin_t *bin = &arena->bins[i];

		malloc_mutex_lock(&bin->lock);
		bstats[i].allocated += bin->stats.allocated;
		bstats[i].nmalloc += bin->stats.nmalloc;
		bstats[i].ndalloc += bin->stats.ndalloc;
		bstats[i].nrequests += bin->stats.nrequests;
		if (config_tcache) {
			bstats[i].nfills += bin->stats.nfills;
			bstats[i].nflushes += bin->stats.nflushes;
		}
		bstats[i].nruns += bin->stats.nruns;
		bstats[i].reruns += bin->stats.reruns;
		bstats[i].curruns += bin->stats.curruns;
		malloc_mutex_unlock(&bin->lock);
	}
}

void
arena_dalloc_large(arena_t *arena, arena_chunk_t *chunk, void *ptr)
{

	if (config_fill || config_stats) {
		size_t pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
		size_t size = chunk->map[pageind-map_bias].bits & ~PAGE_MASK;

		if (config_fill && config_stats && opt_junk)
			memset(ptr, 0x5a, size);
		if (config_stats) {
			arena->stats.ndalloc_large++;
			arena->stats.allocated_large -= size;
			arena->stats.lstats[(size >> LG_PAGE) - 1].ndalloc++;
			arena->stats.lstats[(size >> LG_PAGE) - 1].curruns--;
		}
	}

	arena_run_dalloc(arena, (arena_run_t *)ptr, true);
}

static void
arena_ralloc_large_shrink(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    size_t oldsize, size_t size)
{

	assert(size < oldsize);

	/*
	 * Shrink the run, and make trailing pages available for other
	 * allocations.
	 */
	malloc_mutex_lock(&arena->lock);
	arena_run_trim_tail(arena, chunk, (arena_run_t *)ptr, oldsize, size,
	    true);
	if (config_stats) {
		arena->stats.ndalloc_large++;
		arena->stats.allocated_large -= oldsize;
		arena->stats.lstats[(oldsize >> LG_PAGE) - 1].ndalloc++;
		arena->stats.lstats[(oldsize >> LG_PAGE) - 1].curruns--;

		arena->stats.nmalloc_large++;
		arena->stats.nrequests_large++;
		arena->stats.allocated_large += size;
		arena->stats.lstats[(size >> LG_PAGE) - 1].nmalloc++;
		arena->stats.lstats[(size >> LG_PAGE) - 1].nrequests++;
		arena->stats.lstats[(size >> LG_PAGE) - 1].curruns++;
	}
	malloc_mutex_unlock(&arena->lock);
}

static bool
arena_ralloc_large_grow(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    size_t oldsize, size_t size, size_t extra, bool zero)
{
	size_t pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	size_t npages = oldsize >> LG_PAGE;
	size_t followsize;

	assert(oldsize == (chunk->map[pageind-map_bias].bits & ~PAGE_MASK));

	/* Try to extend the run. */
	assert(size + extra > oldsize);
	malloc_mutex_lock(&arena->lock);
	if (pageind + npages < chunk_npages &&
	    (chunk->map[pageind+npages-map_bias].bits
	    & CHUNK_MAP_ALLOCATED) == 0 && (followsize =
	    chunk->map[pageind+npages-map_bias].bits & ~PAGE_MASK) >= size -
	    oldsize) {
		/*
		 * The next run is available and sufficiently large.  Split the
		 * following run, then merge the first part with the existing
		 * allocation.
		 */
		size_t flag_dirty;
		size_t splitsize = (oldsize + followsize <= size + extra)
		    ? followsize : size + extra - oldsize;
		arena_run_split(arena, (arena_run_t *)((uintptr_t)chunk +
		    ((pageind+npages) << LG_PAGE)), splitsize, true, zero);

		size = oldsize + splitsize;
		npages = size >> LG_PAGE;

		/*
		 * Mark the extended run as dirty if either portion of the run
		 * was dirty before allocation.  This is rather pedantic,
		 * because there's not actually any sequence of events that
		 * could cause the resulting run to be passed to
		 * arena_run_dalloc() with the dirty argument set to false
		 * (which is when dirty flag consistency would really matter).
		 */
		flag_dirty = (chunk->map[pageind-map_bias].bits &
		    CHUNK_MAP_DIRTY) |
		    (chunk->map[pageind+npages-1-map_bias].bits &
		    CHUNK_MAP_DIRTY);
		chunk->map[pageind-map_bias].bits = size | flag_dirty
		    | CHUNK_MAP_LARGE | CHUNK_MAP_ALLOCATED;
		chunk->map[pageind+npages-1-map_bias].bits = flag_dirty |
		    CHUNK_MAP_LARGE | CHUNK_MAP_ALLOCATED;

		if (config_stats) {
			arena->stats.ndalloc_large++;
			arena->stats.allocated_large -= oldsize;
			arena->stats.lstats[(oldsize >> LG_PAGE)
			    - 1].ndalloc++;
			arena->stats.lstats[(oldsize >> LG_PAGE)
			    - 1].curruns--;

			arena->stats.nmalloc_large++;
			arena->stats.nrequests_large++;
			arena->stats.allocated_large += size;
			arena->stats.lstats[(size >> LG_PAGE) - 1].nmalloc++;
			arena->stats.lstats[(size >> LG_PAGE)
			    - 1].nrequests++;
			arena->stats.lstats[(size >> LG_PAGE) - 1].curruns++;
		}
		malloc_mutex_unlock(&arena->lock);
		return (false);
	}
	malloc_mutex_unlock(&arena->lock);

	return (true);
}

/*
 * Try to resize a large allocation, in order to avoid copying.  This will
 * always fail if growing an object, and the following run is already in use.
 */
static bool
arena_ralloc_large(void *ptr, size_t oldsize, size_t size, size_t extra,
    bool zero)
{
	size_t psize;

	psize = PAGE_CEILING(size + extra);
	if (psize == oldsize) {
		/* Same size class. */
		if (config_fill && opt_junk && size < oldsize) {
			memset((void *)((uintptr_t)ptr + size), 0x5a, oldsize -
			    size);
		}
		return (false);
	} else {
		arena_chunk_t *chunk;
		arena_t *arena;

		chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
		arena = chunk->arena;

		if (psize < oldsize) {
			/* Fill before shrinking in order avoid a race. */
			if (config_fill && opt_junk) {
				memset((void *)((uintptr_t)ptr + size), 0x5a,
				    oldsize - size);
			}
			arena_ralloc_large_shrink(arena, chunk, ptr, oldsize,
			    psize);
			return (false);
		} else {
			bool ret = arena_ralloc_large_grow(arena, chunk, ptr,
			    oldsize, PAGE_CEILING(size),
			    psize - PAGE_CEILING(size), zero);
			if (config_fill && ret == false && zero == false &&
			    opt_zero) {
				memset((void *)((uintptr_t)ptr + oldsize), 0,
				    size - oldsize);
			}
			return (ret);
		}
	}
}

void *
arena_ralloc_no_move(void *ptr, size_t oldsize, size_t size, size_t extra,
    bool zero)
{

	/*
	 * Avoid moving the allocation if the size class can be left the same.
	 */
	if (oldsize <= arena_maxclass) {
		if (oldsize <= SMALL_MAXCLASS) {
			assert(arena_bin_info[SMALL_SIZE2BIN(oldsize)].reg_size
			    == oldsize);
			if ((size + extra <= SMALL_MAXCLASS &&
			    SMALL_SIZE2BIN(size + extra) ==
			    SMALL_SIZE2BIN(oldsize)) || (size <= oldsize &&
			    size + extra >= oldsize)) {
				if (config_fill && opt_junk && size < oldsize) {
					memset((void *)((uintptr_t)ptr + size),
					    0x5a, oldsize - size);
				}
				return (ptr);
			}
		} else {
			assert(size <= arena_maxclass);
			if (size + extra > SMALL_MAXCLASS) {
				if (arena_ralloc_large(ptr, oldsize, size,
				    extra, zero) == false)
					return (ptr);
			}
		}
	}

	/* Reallocation would require a move. */
	return (NULL);
}

void *
arena_ralloc(void *ptr, size_t oldsize, size_t size, size_t extra,
    size_t alignment, bool zero, bool try_tcache)
{
	void *ret;
	size_t copysize;

	/* Try to avoid moving the allocation. */
	ret = arena_ralloc_no_move(ptr, oldsize, size, extra, zero);
	if (ret != NULL)
		return (ret);

	/*
	 * size and oldsize are different enough that we need to move the
	 * object.  In that case, fall back to allocating new space and
	 * copying.
	 */
	if (alignment != 0) {
		size_t usize = sa2u(size + extra, alignment);
		if (usize == 0)
			return (NULL);
		ret = ipalloc(usize, alignment, zero);
	} else
		ret = arena_malloc(NULL, size + extra, zero, try_tcache);

	if (ret == NULL) {
		if (extra == 0)
			return (NULL);
		/* Try again, this time without extra. */
		if (alignment != 0) {
			size_t usize = sa2u(size, alignment);
			if (usize == 0)
				return (NULL);
			ret = ipalloc(usize, alignment, zero);
		} else
			ret = arena_malloc(NULL, size, zero, try_tcache);

		if (ret == NULL)
			return (NULL);
	}

	/* Junk/zero-filling were already done by ipalloc()/arena_malloc(). */

	/*
	 * Copy at most size bytes (not size+extra), since the caller has no
	 * expectation that the extra bytes will be reliably preserved.
	 */
	copysize = (size < oldsize) ? size : oldsize;
	VALGRIND_MAKE_MEM_UNDEFINED(ret, copysize);
	memcpy(ret, ptr, copysize);
	iqalloc(ptr);
	return (ret);
}

bool
arena_new(arena_t *arena, unsigned ind)
{
	unsigned i;
	arena_bin_t *bin;

	arena->ind = ind;
	arena->nthreads = 0;

	if (malloc_mutex_init(&arena->lock))
		return (true);

	if (config_stats) {
		memset(&arena->stats, 0, sizeof(arena_stats_t));
		arena->stats.lstats =
		    (malloc_large_stats_t *)base_alloc(nlclasses *
		    sizeof(malloc_large_stats_t));
		if (arena->stats.lstats == NULL)
			return (true);
		memset(arena->stats.lstats, 0, nlclasses *
		    sizeof(malloc_large_stats_t));
		if (config_tcache)
			ql_new(&arena->tcache_ql);
	}

	if (config_prof)
		arena->prof_accumbytes = 0;

	/* Initialize chunks. */
	ql_new(&arena->chunks_dirty);
	arena->spare = NULL;

	arena->nactive = 0;
	arena->ndirty = 0;
	arena->npurgatory = 0;

	arena_avail_tree_new(&arena->runs_avail_clean);
	arena_avail_tree_new(&arena->runs_avail_dirty);

	/* Initialize bins. */
	for (i = 0; i < NBINS; i++) {
		bin = &arena->bins[i];
		if (malloc_mutex_init(&bin->lock))
			return (true);
		bin->runcur = NULL;
		arena_run_tree_new(&bin->runs);
		if (config_stats)
			memset(&bin->stats, 0, sizeof(malloc_bin_stats_t));
	}

	return (false);
}

/*
 * Calculate bin_info->run_size such that it meets the following constraints:
 *
 *   *) bin_info->run_size >= min_run_size
 *   *) bin_info->run_size <= arena_maxclass
 *   *) run header overhead <= RUN_MAX_OVRHD (or header overhead relaxed).
 *   *) bin_info->nregs <= RUN_MAXREGS
 *
 * bin_info->nregs, bin_info->bitmap_offset, and bin_info->reg0_offset are also
 * calculated here, since these settings are all interdependent.
 */
static size_t
bin_info_run_size_calc(arena_bin_info_t *bin_info, size_t min_run_size)
{
	size_t pad_size;
	size_t try_run_size, good_run_size;
	uint32_t try_nregs, good_nregs;
	uint32_t try_hdr_size, good_hdr_size;
	uint32_t try_bitmap_offset, good_bitmap_offset;
	uint32_t try_ctx0_offset, good_ctx0_offset;
	uint32_t try_redzone0_offset, good_redzone0_offset;

	assert(min_run_size >= PAGE);
	assert(min_run_size <= arena_maxclass);

	/*
	 * Determine redzone size based on minimum alignment and minimum
	 * redzone size.  Add padding to the end of the run if it is needed to
	 * align the regions.  The padding allows each redzone to be half the
	 * minimum alignment; without the padding, each redzone would have to
	 * be twice as large in order to maintain alignment.
	 */
	if (config_fill && opt_redzone) {
		size_t align_min = ZU(1) << (ffs(bin_info->reg_size) - 1);
		if (align_min <= REDZONE_MINSIZE) {
			bin_info->redzone_size = REDZONE_MINSIZE;
			pad_size = 0;
		} else {
			bin_info->redzone_size = align_min >> 1;
			pad_size = bin_info->redzone_size;
		}
	} else {
		bin_info->redzone_size = 0;
		pad_size = 0;
	}
	bin_info->reg_interval = bin_info->reg_size +
	    (bin_info->redzone_size << 1);

	/*
	 * Calculate known-valid settings before entering the run_size
	 * expansion loop, so that the first part of the loop always copies
	 * valid settings.
	 *
	 * The do..while loop iteratively reduces the number of regions until
	 * the run header and the regions no longer overlap.  A closed formula
	 * would be quite messy, since there is an interdependency between the
	 * header's mask length and the number of regions.
	 */
	try_run_size = min_run_size;
	try_nregs = ((try_run_size - sizeof(arena_run_t)) /
	    bin_info->reg_interval)
	    + 1; /* Counter-act try_nregs-- in loop. */
	if (try_nregs > RUN_MAXREGS) {
		try_nregs = RUN_MAXREGS
		    + 1; /* Counter-act try_nregs-- in loop. */
	}
	do {
		try_nregs--;
		try_hdr_size = sizeof(arena_run_t);
		/* Pad to a long boundary. */
		try_hdr_size = LONG_CEILING(try_hdr_size);
		try_bitmap_offset = try_hdr_size;
		/* Add space for bitmap. */
		try_hdr_size += bitmap_size(try_nregs);
		if (config_prof && opt_prof && prof_promote == false) {
			/* Pad to a quantum boundary. */
			try_hdr_size = QUANTUM_CEILING(try_hdr_size);
			try_ctx0_offset = try_hdr_size;
			/* Add space for one (prof_ctx_t *) per region. */
			try_hdr_size += try_nregs * sizeof(prof_ctx_t *);
		} else
			try_ctx0_offset = 0;
		try_redzone0_offset = try_run_size - (try_nregs *
		    bin_info->reg_interval) - pad_size;
	} while (try_hdr_size > try_redzone0_offset);

	/* run_size expansion loop. */
	do {
		/*
		 * Copy valid settings before trying more aggressive settings.
		 */
		good_run_size = try_run_size;
		good_nregs = try_nregs;
		good_hdr_size = try_hdr_size;
		good_bitmap_offset = try_bitmap_offset;
		good_ctx0_offset = try_ctx0_offset;
		good_redzone0_offset = try_redzone0_offset;

		/* Try more aggressive settings. */
		try_run_size += PAGE;
		try_nregs = ((try_run_size - sizeof(arena_run_t) - pad_size) /
		    bin_info->reg_interval)
		    + 1; /* Counter-act try_nregs-- in loop. */
		if (try_nregs > RUN_MAXREGS) {
			try_nregs = RUN_MAXREGS
			    + 1; /* Counter-act try_nregs-- in loop. */
		}
		do {
			try_nregs--;
			try_hdr_size = sizeof(arena_run_t);
			/* Pad to a long boundary. */
			try_hdr_size = LONG_CEILING(try_hdr_size);
			try_bitmap_offset = try_hdr_size;
			/* Add space for bitmap. */
			try_hdr_size += bitmap_size(try_nregs);
			if (config_prof && opt_prof && prof_promote == false) {
				/* Pad to a quantum boundary. */
				try_hdr_size = QUANTUM_CEILING(try_hdr_size);
				try_ctx0_offset = try_hdr_size;
				/*
				 * Add space for one (prof_ctx_t *) per region.
				 */
				try_hdr_size += try_nregs *
				    sizeof(prof_ctx_t *);
			}
			try_redzone0_offset = try_run_size - (try_nregs *
			    bin_info->reg_interval) - pad_size;
		} while (try_hdr_size > try_redzone0_offset);
	} while (try_run_size <= arena_maxclass
	    && try_run_size <= arena_maxclass
	    && RUN_MAX_OVRHD * (bin_info->reg_interval << 3) >
	    RUN_MAX_OVRHD_RELAX
	    && (try_redzone0_offset << RUN_BFP) > RUN_MAX_OVRHD * try_run_size
	    && try_nregs < RUN_MAXREGS);

	assert(good_hdr_size <= good_redzone0_offset);

	/* Copy final settings. */
	bin_info->run_size = good_run_size;
	bin_info->nregs = good_nregs;
	bin_info->bitmap_offset = good_bitmap_offset;
	bin_info->ctx0_offset = good_ctx0_offset;
	bin_info->reg0_offset = good_redzone0_offset + bin_info->redzone_size;

	assert(bin_info->reg0_offset - bin_info->redzone_size + (bin_info->nregs
	    * bin_info->reg_interval) + pad_size == bin_info->run_size);

	return (good_run_size);
}

static void
bin_info_init(void)
{
	arena_bin_info_t *bin_info;
	size_t prev_run_size = PAGE;

#define	SIZE_CLASS(bin, delta, size)					\
	bin_info = &arena_bin_info[bin];				\
	bin_info->reg_size = size;					\
	prev_run_size = bin_info_run_size_calc(bin_info, prev_run_size);\
	bitmap_info_init(&bin_info->bitmap_info, bin_info->nregs);
	SIZE_CLASSES
#undef SIZE_CLASS
}

void
arena_boot(void)
{
	size_t header_size;
	unsigned i;

	/*
	 * Compute the header size such that it is large enough to contain the
	 * page map.  The page map is biased to omit entries for the header
	 * itself, so some iteration is necessary to compute the map bias.
	 *
	 * 1) Compute safe header_size and map_bias values that include enough
	 *    space for an unbiased page map.
	 * 2) Refine map_bias based on (1) to omit the header pages in the page
	 *    map.  The resulting map_bias may be one too small.
	 * 3) Refine map_bias based on (2).  The result will be >= the result
	 *    from (2), and will always be correct.
	 */
	map_bias = 0;
	for (i = 0; i < 3; i++) {
		header_size = offsetof(arena_chunk_t, map) +
		    (sizeof(arena_chunk_map_t) * (chunk_npages-map_bias));
		map_bias = (header_size >> LG_PAGE) + ((header_size & PAGE_MASK)
		    != 0);
	}
	assert(map_bias > 0);

	arena_maxclass = chunksize - (map_bias << LG_PAGE);

	bin_info_init();
}

void
arena_prefork(arena_t *arena)
{
	unsigned i;

	malloc_mutex_prefork(&arena->lock);
	for (i = 0; i < NBINS; i++)
		malloc_mutex_prefork(&arena->bins[i].lock);
}

void
arena_postfork_parent(arena_t *arena)
{
	unsigned i;

	for (i = 0; i < NBINS; i++)
		malloc_mutex_postfork_parent(&arena->bins[i].lock);
	malloc_mutex_postfork_parent(&arena->lock);
}

void
arena_postfork_child(arena_t *arena)
{
	unsigned i;

	for (i = 0; i < NBINS; i++)
		malloc_mutex_postfork_child(&arena->bins[i].lock);
	malloc_mutex_postfork_child(&arena->lock);
}
