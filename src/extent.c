#define JEMALLOC_EXTENT_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/ph.h"


/******************************************************************************/
/* Data. */

rtree_t		extents_rtree;

static const bitmap_info_t extents_bitmap_info =
    BITMAP_INFO_INITIALIZER(NPSIZES+1);

static void	*extent_alloc_default(extent_hooks_t *extent_hooks,
    void *new_addr, size_t size, size_t alignment, bool *zero, bool *commit,
    unsigned arena_ind);
static bool	extent_dalloc_default(extent_hooks_t *extent_hooks, void *addr,
    size_t size, bool committed, unsigned arena_ind);
static bool	extent_commit_default(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t offset, size_t length, unsigned arena_ind);
static bool	extent_decommit_default(extent_hooks_t *extent_hooks,
    void *addr, size_t size, size_t offset, size_t length, unsigned arena_ind);
#ifdef PAGES_CAN_PURGE_LAZY
static bool	extent_purge_lazy_default(extent_hooks_t *extent_hooks,
    void *addr, size_t size, size_t offset, size_t length, unsigned arena_ind);
#endif
#ifdef PAGES_CAN_PURGE_FORCED
static bool	extent_purge_forced_default(extent_hooks_t *extent_hooks,
    void *addr, size_t size, size_t offset, size_t length, unsigned arena_ind);
#endif
#ifdef JEMALLOC_MAPS_COALESCE
static bool	extent_split_default(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t size_a, size_t size_b, bool committed,
    unsigned arena_ind);
static bool	extent_merge_default(extent_hooks_t *extent_hooks, void *addr_a,
    size_t size_a, void *addr_b, size_t size_b, bool committed,
    unsigned arena_ind);
#endif

const extent_hooks_t	extent_hooks_default = {
	extent_alloc_default,
	extent_dalloc_default,
	extent_commit_default,
	extent_decommit_default
#ifdef PAGES_CAN_PURGE_LAZY
	,
	extent_purge_lazy_default
#else
	,
	NULL
#endif
#ifdef PAGES_CAN_PURGE_FORCED
	,
	extent_purge_forced_default
#else
	,
	NULL
#endif
#ifdef JEMALLOC_MAPS_COALESCE
	,
	extent_split_default,
	extent_merge_default
#endif
};

/* Used exclusively for gdump triggering. */
static atomic_zu_t curpages;
static atomic_zu_t highpages;

/******************************************************************************/
/*
 * Function prototypes for static functions that are referenced prior to
 * definition.
 */

static void extent_deregister(tsdn_t *tsdn, extent_t *extent);
static extent_t *extent_recycle(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extents_t *extents, void *new_addr,
    size_t usize, size_t pad, size_t alignment, bool slab, szind_t szind,
    bool *zero, bool *commit);
static extent_t *extent_try_coalesce(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, rtree_ctx_t *rtree_ctx, extents_t *extents,
    extent_t *extent, bool *coalesced);
static void extent_record(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extents_t *extents, extent_t *extent);

/******************************************************************************/

rb_gen(UNUSED, extent_avail_, extent_tree_t, extent_t, rb_link,
    extent_esnead_comp)

extent_t *
extent_alloc(tsdn_t *tsdn, arena_t *arena) {
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	malloc_mutex_lock(tsdn, &arena->extent_avail_mtx);
	extent_t *extent = extent_avail_first(&arena->extent_avail);
	if (extent == NULL) {
		malloc_mutex_unlock(tsdn, &arena->extent_avail_mtx);
		return base_alloc_extent(tsdn, arena->base);
	}
	extent_avail_remove(&arena->extent_avail, extent);
	malloc_mutex_unlock(tsdn, &arena->extent_avail_mtx);
	return extent;
}

void
extent_dalloc(tsdn_t *tsdn, arena_t *arena, extent_t *extent) {
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	malloc_mutex_lock(tsdn, &arena->extent_avail_mtx);
	extent_avail_insert(&arena->extent_avail, extent);
	malloc_mutex_unlock(tsdn, &arena->extent_avail_mtx);
}

extent_hooks_t *
extent_hooks_get(arena_t *arena) {
	return base_extent_hooks_get(arena->base);
}

extent_hooks_t *
extent_hooks_set(arena_t *arena, extent_hooks_t *extent_hooks) {
	return base_extent_hooks_set(arena->base, extent_hooks);
}

static void
extent_hooks_assure_initialized(arena_t *arena,
    extent_hooks_t **r_extent_hooks) {
	if (*r_extent_hooks == EXTENT_HOOKS_INITIALIZER) {
		*r_extent_hooks = extent_hooks_get(arena);
	}
}

#ifndef JEMALLOC_JET
static
#endif
size_t
extent_size_quantize_floor(size_t size) {
	size_t ret;
	pszind_t pind;

	assert(size > 0);
	assert((size & PAGE_MASK) == 0);

	pind = psz2ind(size - large_pad + 1);
	if (pind == 0) {
		/*
		 * Avoid underflow.  This short-circuit would also do the right
		 * thing for all sizes in the range for which there are
		 * PAGE-spaced size classes, but it's simplest to just handle
		 * the one case that would cause erroneous results.
		 */
		return size;
	}
	ret = pind2sz(pind - 1) + large_pad;
	assert(ret <= size);
	return ret;
}

#ifndef JEMALLOC_JET
static
#endif
size_t
extent_size_quantize_ceil(size_t size) {
	size_t ret;

	assert(size > 0);
	assert(size - large_pad <= LARGE_MAXCLASS);
	assert((size & PAGE_MASK) == 0);

	ret = extent_size_quantize_floor(size);
	if (ret < size) {
		/*
		 * Skip a quantization that may have an adequately large extent,
		 * because under-sized extents may be mixed in.  This only
		 * happens when an unusual size is requested, i.e. for aligned
		 * allocation, and is just one of several places where linear
		 * search would potentially find sufficiently aligned available
		 * memory somewhere lower.
		 */
		ret = pind2sz(psz2ind(ret - large_pad + 1)) + large_pad;
	}
	return ret;
}

/* Generate pairing heap functions. */
ph_gen(, extent_heap_, extent_heap_t, extent_t, ph_link, extent_snad_comp)

bool
extents_init(tsdn_t *tsdn, extents_t *extents, extent_state_t state,
    bool delay_coalesce) {
	if (malloc_mutex_init(&extents->mtx, "extents", WITNESS_RANK_EXTENTS)) {
		return true;
	}
	for (unsigned i = 0; i < NPSIZES+1; i++) {
		extent_heap_new(&extents->heaps[i]);
	}
	bitmap_init(extents->bitmap, &extents_bitmap_info, true);
	extent_list_init(&extents->lru);
	atomic_store_zu(&extents->npages, 0, ATOMIC_RELAXED);
	extents->state = state;
	extents->delay_coalesce = delay_coalesce;
	return false;
}

extent_state_t
extents_state_get(const extents_t *extents) {
	return extents->state;
}

size_t
extents_npages_get(extents_t *extents) {
	return atomic_load_zu(&extents->npages, ATOMIC_RELAXED);
}

static void
extents_insert_locked(tsdn_t *tsdn, extents_t *extents, extent_t *extent,
    bool preserve_lru) {
	malloc_mutex_assert_owner(tsdn, &extents->mtx);
	assert(extent_state_get(extent) == extents->state);

	size_t size = extent_size_get(extent);
	size_t psz = extent_size_quantize_floor(size);
	pszind_t pind = psz2ind(psz);
	if (extent_heap_empty(&extents->heaps[pind])) {
		bitmap_unset(extents->bitmap, &extents_bitmap_info,
		    (size_t)pind);
	}
	extent_heap_insert(&extents->heaps[pind], extent);
	if (!preserve_lru) {
		extent_list_append(&extents->lru, extent);
	}
	size_t npages = size >> LG_PAGE;
	/*
	 * All modifications to npages hold the mutex (as asserted above), so we
	 * don't need an atomic fetch-add; we can get by with a load followed by
	 * a store.
	 */
	size_t cur_extents_npages =
	    atomic_load_zu(&extents->npages, ATOMIC_RELAXED);
	atomic_store_zu(&extents->npages, cur_extents_npages + npages,
	    ATOMIC_RELAXED);
}

static void
extents_remove_locked(tsdn_t *tsdn, extents_t *extents, extent_t *extent,
    bool preserve_lru) {
	malloc_mutex_assert_owner(tsdn, &extents->mtx);
	assert(extent_state_get(extent) == extents->state);

	size_t size = extent_size_get(extent);
	size_t psz = extent_size_quantize_floor(size);
	pszind_t pind = psz2ind(psz);
	extent_heap_remove(&extents->heaps[pind], extent);
	if (extent_heap_empty(&extents->heaps[pind])) {
		bitmap_set(extents->bitmap, &extents_bitmap_info,
		    (size_t)pind);
	}
	if (!preserve_lru) {
		extent_list_remove(&extents->lru, extent);
	}
	size_t npages = size >> LG_PAGE;
	/*
	 * As in extents_insert_locked, we hold extents->mtx and so don't need
	 * atomic operations for updating extents->npages.
	 */
	size_t cur_extents_npages =
	    atomic_load_zu(&extents->npages, ATOMIC_RELAXED);
	assert(cur_extents_npages >= npages);
	atomic_store_zu(&extents->npages,
	    cur_extents_npages - (size >> LG_PAGE), ATOMIC_RELAXED);
}

/* Do any-best-fit extent selection, i.e. select any extent that best fits. */
static extent_t *
extents_best_fit_locked(tsdn_t *tsdn, arena_t *arena, extents_t *extents,
    size_t size) {
	pszind_t pind = psz2ind(extent_size_quantize_ceil(size));
	pszind_t i = (pszind_t)bitmap_ffu(extents->bitmap, &extents_bitmap_info,
	    (size_t)pind);
	if (i < NPSIZES+1) {
		assert(!extent_heap_empty(&extents->heaps[i]));
		extent_t *extent = extent_heap_any(&extents->heaps[i]);
		assert(extent_size_get(extent) >= size);
		return extent;
	}

	return NULL;
}

/*
 * Do first-fit extent selection, i.e. select the oldest/lowest extent that is
 * large enough.
 */
static extent_t *
extents_first_fit_locked(tsdn_t *tsdn, arena_t *arena, extents_t *extents,
    size_t size) {
	extent_t *ret = NULL;

	pszind_t pind = psz2ind(extent_size_quantize_ceil(size));
	for (pszind_t i = (pszind_t)bitmap_ffu(extents->bitmap,
	    &extents_bitmap_info, (size_t)pind); i < NPSIZES+1; i =
	    (pszind_t)bitmap_ffu(extents->bitmap, &extents_bitmap_info,
	    (size_t)i+1)) {
		assert(!extent_heap_empty(&extents->heaps[i]));
		extent_t *extent = extent_heap_first(&extents->heaps[i]);
		assert(extent_size_get(extent) >= size);
		if (ret == NULL || extent_snad_comp(extent, ret) < 0) {
			ret = extent;
		}
		if (i == NPSIZES) {
			break;
		}
		assert(i < NPSIZES);
	}

	return ret;
}

/*
 * Do {best,first}-fit extent selection, where the selection policy choice is
 * based on extents->delay_coalesce.  Best-fit selection requires less
 * searching, but its layout policy is less stable and may cause higher virtual
 * memory fragmentation as a side effect.
 */
static extent_t *
extents_fit_locked(tsdn_t *tsdn, arena_t *arena, extents_t *extents,
    size_t size) {
	malloc_mutex_assert_owner(tsdn, &extents->mtx);

	return extents->delay_coalesce ? extents_best_fit_locked(tsdn, arena,
	    extents, size) : extents_first_fit_locked(tsdn, arena, extents,
	    size);
}

static bool
extent_try_delayed_coalesce(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, rtree_ctx_t *rtree_ctx, extents_t *extents,
    extent_t *extent) {
	extent_state_set(extent, extent_state_active);
	bool coalesced;
	extent = extent_try_coalesce(tsdn, arena, r_extent_hooks, rtree_ctx,
	    extents, extent, &coalesced);
	extent_state_set(extent, extents_state_get(extents));

	if (!coalesced) {
		return true;
	}
	extents_insert_locked(tsdn, extents, extent, true);
	return false;
}

extent_t *
extents_alloc(tsdn_t *tsdn, arena_t *arena, extent_hooks_t **r_extent_hooks,
    extents_t *extents, void *new_addr, size_t size, size_t pad,
    size_t alignment, bool slab, szind_t szind, bool *zero, bool *commit) {
	assert(size + pad != 0);
	assert(alignment != 0);
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	return extent_recycle(tsdn, arena, r_extent_hooks, extents, new_addr,
	    size, pad, alignment, slab, szind, zero, commit);
}

void
extents_dalloc(tsdn_t *tsdn, arena_t *arena, extent_hooks_t **r_extent_hooks,
    extents_t *extents, extent_t *extent) {
	assert(extent_base_get(extent) != NULL);
	assert(extent_size_get(extent) != 0);
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	extent_addr_set(extent, extent_base_get(extent));
	extent_zeroed_set(extent, false);

	extent_record(tsdn, arena, r_extent_hooks, extents, extent);
}

extent_t *
extents_evict(tsdn_t *tsdn, arena_t *arena, extent_hooks_t **r_extent_hooks,
    extents_t *extents, size_t npages_min) {
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

	malloc_mutex_lock(tsdn, &extents->mtx);

	/*
	 * Get the LRU coalesced extent, if any.  If coalescing was delayed,
	 * the loop will iterate until the LRU extent is fully coalesced.
	 */
	extent_t *extent;
	while (true) {
		/* Get the LRU extent, if any. */
		extent = extent_list_first(&extents->lru);
		if (extent == NULL) {
			goto label_return;
		}
		/* Check the eviction limit. */
		size_t npages = extent_size_get(extent) >> LG_PAGE;
		size_t extents_npages = atomic_load_zu(&extents->npages,
		    ATOMIC_RELAXED);
		if (extents_npages - npages < npages_min) {
			extent = NULL;
			goto label_return;
		}
		extents_remove_locked(tsdn, extents, extent, false);
		if (!extents->delay_coalesce) {
			break;
		}
		/* Try to coalesce. */
		if (extent_try_delayed_coalesce(tsdn, arena, r_extent_hooks,
		    rtree_ctx, extents, extent)) {
			break;
		}
		/*
		 * The LRU extent was just coalesced and the result placed in
		 * the LRU at its neighbor's position.  Start over.
		 */
	}

	/*
	 * Either mark the extent active or deregister it to protect against
	 * concurrent operations.
	 */
	switch (extents_state_get(extents)) {
	case extent_state_active:
		not_reached();
	case extent_state_dirty:
	case extent_state_muzzy:
		extent_state_set(extent, extent_state_active);
		break;
	case extent_state_retained:
		extent_deregister(tsdn, extent);
		break;
	default:
		not_reached();
	}

label_return:
	malloc_mutex_unlock(tsdn, &extents->mtx);
	return extent;
}

static void
extents_leak(tsdn_t *tsdn, arena_t *arena, extent_hooks_t **r_extent_hooks,
    extents_t *extents, extent_t *extent) {
	/*
	 * Leak extent after making sure its pages have already been purged, so
	 * that this is only a virtual memory leak.
	 */
	if (extents_state_get(extents) == extent_state_dirty) {
		if (extent_purge_lazy_wrapper(tsdn, arena, r_extent_hooks,
		    extent, 0, extent_size_get(extent))) {
			extent_purge_forced_wrapper(tsdn, arena, r_extent_hooks,
			    extent, 0, extent_size_get(extent));
		}
	}
	extent_dalloc(tsdn, arena, extent);
}

void
extents_prefork(tsdn_t *tsdn, extents_t *extents) {
	malloc_mutex_prefork(tsdn, &extents->mtx);
}

void
extents_postfork_parent(tsdn_t *tsdn, extents_t *extents) {
	malloc_mutex_postfork_parent(tsdn, &extents->mtx);
}

void
extents_postfork_child(tsdn_t *tsdn, extents_t *extents) {
	malloc_mutex_postfork_child(tsdn, &extents->mtx);
}

static void
extent_deactivate_locked(tsdn_t *tsdn, arena_t *arena, extents_t *extents,
    extent_t *extent, bool preserve_lru) {
	assert(extent_arena_get(extent) == arena);
	assert(extent_state_get(extent) == extent_state_active);

	extent_state_set(extent, extents_state_get(extents));
	extents_insert_locked(tsdn, extents, extent, preserve_lru);
}

static void
extent_deactivate(tsdn_t *tsdn, arena_t *arena, extents_t *extents,
    extent_t *extent, bool preserve_lru) {
	malloc_mutex_lock(tsdn, &extents->mtx);
	extent_deactivate_locked(tsdn, arena, extents, extent, preserve_lru);
	malloc_mutex_unlock(tsdn, &extents->mtx);
}

static void
extent_activate_locked(tsdn_t *tsdn, arena_t *arena, extents_t *extents,
    extent_t *extent, bool preserve_lru) {
	assert(extent_arena_get(extent) == arena);
	assert(extent_state_get(extent) == extents_state_get(extents));

	extents_remove_locked(tsdn, extents, extent, preserve_lru);
	extent_state_set(extent, extent_state_active);
}

static bool
extent_rtree_acquire(tsdn_t *tsdn, rtree_ctx_t *rtree_ctx,
    const extent_t *extent, bool dependent, bool init_missing,
    rtree_leaf_elm_t **r_elm_a, rtree_leaf_elm_t **r_elm_b) {
	*r_elm_a = rtree_leaf_elm_acquire(tsdn, &extents_rtree, rtree_ctx,
	    (uintptr_t)extent_base_get(extent), dependent, init_missing);
	if (!dependent && *r_elm_a == NULL) {
		return true;
	}
	assert(*r_elm_a != NULL);

	if (extent_size_get(extent) > PAGE) {
		*r_elm_b = rtree_leaf_elm_acquire(tsdn, &extents_rtree,
		    rtree_ctx, (uintptr_t)extent_last_get(extent), dependent,
		    init_missing);
		if (!dependent && *r_elm_b == NULL) {
			rtree_leaf_elm_release(tsdn, &extents_rtree, *r_elm_a);
			return true;
		}
		assert(*r_elm_b != NULL);
	} else {
		*r_elm_b = NULL;
	}

	return false;
}

static void
extent_rtree_write_acquired(tsdn_t *tsdn, rtree_leaf_elm_t *elm_a,
    rtree_leaf_elm_t *elm_b, extent_t *extent, szind_t szind, bool slab) {
	rtree_leaf_elm_write(tsdn, &extents_rtree, elm_a, true, extent, szind,
	    slab);
	if (elm_b != NULL) {
		rtree_leaf_elm_write(tsdn, &extents_rtree, elm_b, true, extent,
		    szind, slab);
	}
}

static void
extent_rtree_release(tsdn_t *tsdn, rtree_leaf_elm_t *elm_a,
    rtree_leaf_elm_t *elm_b) {
	rtree_leaf_elm_release(tsdn, &extents_rtree, elm_a);
	if (elm_b != NULL) {
		rtree_leaf_elm_release(tsdn, &extents_rtree, elm_b);
	}
}

static void
extent_interior_register(tsdn_t *tsdn, rtree_ctx_t *rtree_ctx, extent_t *extent,
    szind_t szind) {
	assert(extent_slab_get(extent));

	/* Register interior. */
	for (size_t i = 1; i < (extent_size_get(extent) >> LG_PAGE) - 1; i++) {
		rtree_write(tsdn, &extents_rtree, rtree_ctx,
		    (uintptr_t)extent_base_get(extent) + (uintptr_t)(i <<
		    LG_PAGE), extent, szind, true);
	}
}

static void
extent_gdump_add(tsdn_t *tsdn, const extent_t *extent) {
	cassert(config_prof);
	/* prof_gdump() requirement. */
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	if (opt_prof && extent_state_get(extent) == extent_state_active) {
		size_t nadd = extent_size_get(extent) >> LG_PAGE;
		size_t cur = atomic_fetch_add_zu(&curpages, nadd,
		    ATOMIC_RELAXED) + nadd;
		size_t high = atomic_load_zu(&highpages, ATOMIC_RELAXED);
		while (cur > high && !atomic_compare_exchange_weak_zu(
		    &highpages, &high, cur, ATOMIC_RELAXED, ATOMIC_RELAXED)) {
			/*
			 * Don't refresh cur, because it may have decreased
			 * since this thread lost the highpages update race.
			 * Note that high is updated in case of CAS failure.
			 */
		}
		if (cur > high && prof_gdump_get_unlocked()) {
			prof_gdump(tsdn);
		}
	}
}

static void
extent_gdump_sub(tsdn_t *tsdn, const extent_t *extent) {
	cassert(config_prof);

	if (opt_prof && extent_state_get(extent) == extent_state_active) {
		size_t nsub = extent_size_get(extent) >> LG_PAGE;
		assert(atomic_load_zu(&curpages, ATOMIC_RELAXED) >= nsub);
		atomic_fetch_sub_zu(&curpages, nsub, ATOMIC_RELAXED);
	}
}

static bool
extent_register_impl(tsdn_t *tsdn, extent_t *extent, bool gdump_add) {
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	rtree_leaf_elm_t *elm_a, *elm_b;

	if (extent_rtree_acquire(tsdn, rtree_ctx, extent, false, true, &elm_a,
	    &elm_b)) {
		return true;
	}
	szind_t szind = extent_szind_get_maybe_invalid(extent);
	bool slab = extent_slab_get(extent);
	extent_rtree_write_acquired(tsdn, elm_a, elm_b, extent, szind, slab);
	if (slab) {
		extent_interior_register(tsdn, rtree_ctx, extent, szind);
	}
	extent_rtree_release(tsdn, elm_a, elm_b);

	if (config_prof && gdump_add) {
		extent_gdump_add(tsdn, extent);
	}

	return false;
}

static bool
extent_register(tsdn_t *tsdn, extent_t *extent) {
	return extent_register_impl(tsdn, extent, true);
}

static bool
extent_register_no_gdump_add(tsdn_t *tsdn, extent_t *extent) {
	return extent_register_impl(tsdn, extent, false);
}

static void
extent_reregister(tsdn_t *tsdn, extent_t *extent) {
	bool err = extent_register(tsdn, extent);
	assert(!err);
}

static void
extent_interior_deregister(tsdn_t *tsdn, rtree_ctx_t *rtree_ctx,
    extent_t *extent) {
	size_t i;

	assert(extent_slab_get(extent));

	for (i = 1; i < (extent_size_get(extent) >> LG_PAGE) - 1; i++) {
		rtree_clear(tsdn, &extents_rtree, rtree_ctx,
		    (uintptr_t)extent_base_get(extent) + (uintptr_t)(i <<
		    LG_PAGE));
	}
}

static void
extent_deregister(tsdn_t *tsdn, extent_t *extent) {
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	rtree_leaf_elm_t *elm_a, *elm_b;

	extent_rtree_acquire(tsdn, rtree_ctx, extent, true, false, &elm_a,
	    &elm_b);
	extent_rtree_write_acquired(tsdn, elm_a, elm_b, NULL, NSIZES, false);
	if (extent_slab_get(extent)) {
		extent_interior_deregister(tsdn, rtree_ctx, extent);
		extent_slab_set(extent, false);
	}
	extent_rtree_release(tsdn, elm_a, elm_b);

	if (config_prof) {
		extent_gdump_sub(tsdn, extent);
	}
}

static extent_t *
extent_recycle_extract(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, rtree_ctx_t *rtree_ctx, extents_t *extents,
    bool locked, void *new_addr, size_t size, size_t pad, size_t alignment,
    bool slab, bool *zero, bool *commit) {
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, locked ? 1 : 0);
	if (locked) {
		malloc_mutex_assert_owner(tsdn, &extents->mtx);
	}
	assert(alignment > 0);
	if (config_debug && new_addr != NULL) {
		/*
		 * Non-NULL new_addr has two use cases:
		 *
		 *   1) Recycle a known-extant extent, e.g. during purging.
		 *   2) Perform in-place expanding reallocation.
		 *
		 * Regardless of use case, new_addr must either refer to a
		 * non-existing extent, or to the base of an extant extent,
		 * since only active slabs support interior lookups (which of
		 * course cannot be recycled).
		 */
		assert(PAGE_ADDR2BASE(new_addr) == new_addr);
		assert(pad == 0);
		assert(alignment <= PAGE);
	}

	size_t esize = size + pad;
	size_t alloc_size = esize + PAGE_CEILING(alignment) - PAGE;
	/* Beware size_t wrap-around. */
	if (alloc_size < esize) {
		return NULL;
	}
	if (!locked) {
		malloc_mutex_lock(tsdn, &extents->mtx);
	}
	extent_hooks_assure_initialized(arena, r_extent_hooks);
	extent_t *extent;
	if (new_addr != NULL) {
		rtree_leaf_elm_t *elm = rtree_leaf_elm_acquire(tsdn,
		    &extents_rtree, rtree_ctx, (uintptr_t)new_addr, false,
		    false);
		if (elm != NULL) {
			extent = rtree_leaf_elm_extent_read(tsdn,
			   &extents_rtree, elm, true, true);
			if (extent != NULL) {
				assert(extent_base_get(extent) == new_addr);
				if (extent_arena_get(extent) != arena ||
				    extent_size_get(extent) < esize ||
				    extent_state_get(extent) !=
				    extents_state_get(extents)) {
					extent = NULL;
				}
			}
			rtree_leaf_elm_release(tsdn, &extents_rtree, elm);
		} else {
			extent = NULL;
		}
	} else {
		extent = extents_fit_locked(tsdn, arena, extents, alloc_size);
	}
	if (extent == NULL) {
		if (!locked) {
			malloc_mutex_unlock(tsdn, &extents->mtx);
		}
		return NULL;
	}

	extent_activate_locked(tsdn, arena, extents, extent, false);
	if (!locked) {
		malloc_mutex_unlock(tsdn, &extents->mtx);
	}

	if (extent_zeroed_get(extent)) {
		*zero = true;
	}
	if (extent_committed_get(extent)) {
		*commit = true;
	}

	return extent;
}

static extent_t *
extent_recycle_split(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, rtree_ctx_t *rtree_ctx, extents_t *extents,
    void *new_addr, size_t size, size_t pad, size_t alignment, bool slab,
    szind_t szind, extent_t *extent) {
	size_t esize = size + pad;
	size_t leadsize = ALIGNMENT_CEILING((uintptr_t)extent_base_get(extent),
	    PAGE_CEILING(alignment)) - (uintptr_t)extent_base_get(extent);
	assert(new_addr == NULL || leadsize == 0);
	assert(extent_size_get(extent) >= leadsize + esize);
	size_t trailsize = extent_size_get(extent) - leadsize - esize;

	/* Split the lead. */
	if (leadsize != 0) {
		extent_t *lead = extent;
		extent = extent_split_wrapper(tsdn, arena, r_extent_hooks,
		    lead, leadsize, NSIZES, false, esize + trailsize, szind,
		    slab);
		if (extent == NULL) {
			extent_deregister(tsdn, lead);
			extents_leak(tsdn, arena, r_extent_hooks, extents,
			    lead);
			return NULL;
		}
		extent_deactivate(tsdn, arena, extents, lead, false);
	}

	/* Split the trail. */
	if (trailsize != 0) {
		extent_t *trail = extent_split_wrapper(tsdn, arena,
		    r_extent_hooks, extent, esize, szind, slab, trailsize,
		    NSIZES, false);
		if (trail == NULL) {
			extent_deregister(tsdn, extent);
			extents_leak(tsdn, arena, r_extent_hooks, extents,
			    extent);
			return NULL;
		}
		extent_deactivate(tsdn, arena, extents, trail, false);
	} else if (leadsize == 0) {
		/*
		 * Splitting causes szind to be set as a side effect, but no
		 * splitting occurred.
		 */
		extent_szind_set(extent, szind);
		if (szind != NSIZES) {
			rtree_szind_slab_update(tsdn, &extents_rtree, rtree_ctx,
			    (uintptr_t)extent_addr_get(extent), szind, slab);
			if (slab && extent_size_get(extent) > PAGE) {
				rtree_szind_slab_update(tsdn, &extents_rtree,
				    rtree_ctx,
				    (uintptr_t)extent_past_get(extent) -
				    (uintptr_t)PAGE, szind, slab);
			}
		}
	}

	return extent;
}

static extent_t *
extent_recycle(tsdn_t *tsdn, arena_t *arena, extent_hooks_t **r_extent_hooks,
    extents_t *extents, void *new_addr, size_t size, size_t pad,
    size_t alignment, bool slab, szind_t szind, bool *zero, bool *commit) {
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);
	assert(new_addr == NULL || !slab);
	assert(pad == 0 || !slab);
	assert(!*zero || !slab);

	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

	extent_t *extent = extent_recycle_extract(tsdn, arena, r_extent_hooks,
	    rtree_ctx, extents, false, new_addr, size, pad, alignment, slab,
	    zero, commit);
	if (extent == NULL) {
		return NULL;
	}

	extent = extent_recycle_split(tsdn, arena, r_extent_hooks, rtree_ctx,
	    extents, new_addr, size, pad, alignment, slab, szind, extent);
	if (extent == NULL) {
		return NULL;
	}

	if (*commit && !extent_committed_get(extent)) {
		if (extent_commit_wrapper(tsdn, arena, r_extent_hooks, extent,
		    0, extent_size_get(extent))) {
			extent_record(tsdn, arena, r_extent_hooks, extents,
			    extent);
			return NULL;
		}
		extent_zeroed_set(extent, true);
	}

	if (pad != 0) {
		extent_addr_randomize(tsdn, extent, alignment);
	}
	assert(extent_state_get(extent) == extent_state_active);
	if (slab) {
		extent_slab_set(extent, slab);
		extent_interior_register(tsdn, rtree_ctx, extent, szind);
	}

	if (*zero) {
		void *addr = extent_base_get(extent);
		size_t size = extent_size_get(extent);
		if (!extent_zeroed_get(extent)) {
			if (pages_purge_forced(addr, size)) {
				memset(addr, 0, size);
			}
		} else if (config_debug) {
			size_t *p = (size_t *)(uintptr_t)addr;
			for (size_t i = 0; i < size / sizeof(size_t); i++) {
				assert(p[i] == 0);
			}
		}
	}
	return extent;
}

/*
 * If the caller specifies (!*zero), it is still possible to receive zeroed
 * memory, in which case *zero is toggled to true.  arena_extent_alloc() takes
 * advantage of this to avoid demanding zeroed extents, but taking advantage of
 * them if they are returned.
 */
static void *
extent_alloc_core(tsdn_t *tsdn, arena_t *arena, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, dss_prec_t dss_prec) {
	void *ret;

	assert(size != 0);
	assert(alignment != 0);

	/* "primary" dss. */
	if (have_dss && dss_prec == dss_prec_primary && (ret =
	    extent_alloc_dss(tsdn, arena, new_addr, size, alignment, zero,
	    commit)) != NULL) {
		return ret;
	}
	/* mmap. */
	if ((ret = extent_alloc_mmap(new_addr, size, alignment, zero, commit))
	    != NULL) {
		return ret;
	}
	/* "secondary" dss. */
	if (have_dss && dss_prec == dss_prec_secondary && (ret =
	    extent_alloc_dss(tsdn, arena, new_addr, size, alignment, zero,
	    commit)) != NULL) {
		return ret;
	}

	/* All strategies for allocation failed. */
	return NULL;
}

static void *
extent_alloc_default_impl(tsdn_t *tsdn, arena_t *arena, void *new_addr,
    size_t size, size_t alignment, bool *zero, bool *commit) {
	void *ret;

	ret = extent_alloc_core(tsdn, arena, new_addr, size, alignment, zero,
	    commit, (dss_prec_t)atomic_load_u(&arena->dss_prec,
	    ATOMIC_RELAXED));
	return ret;
}

static void *
extent_alloc_default(extent_hooks_t *extent_hooks, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, unsigned arena_ind) {
	tsdn_t *tsdn;
	arena_t *arena;

	assert(extent_hooks == &extent_hooks_default);

	tsdn = tsdn_fetch();
	arena = arena_get(tsdn, arena_ind, false);
	/*
	 * The arena we're allocating on behalf of must have been initialized
	 * already.
	 */
	assert(arena != NULL);

	return extent_alloc_default_impl(tsdn, arena, new_addr, size,
	    alignment, zero, commit);
}

/*
 * If virtual memory is retained, create increasingly larger extents from which
 * to split requested extents in order to limit the total number of disjoint
 * virtual memory ranges retained by each arena.
 */
static extent_t *
extent_grow_retained(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, void *new_addr, size_t size, size_t pad,
    size_t alignment, bool slab, szind_t szind, bool *zero, bool *commit) {
	assert(pad == 0 || !slab);
	assert(!*zero || !slab);

	/*
	 * Check whether the next extent size in the series would be large
	 * enough to satisfy this request.  If no, just bail, so that e.g. a
	 * series of unsatisfiable allocation requests doesn't cause unused
	 * extent creation as a side effect.
	 */
	size_t esize = size + pad;
	size_t alloc_size = pind2sz(atomic_load_u(&arena->extent_grow_next,
	    ATOMIC_RELAXED));
	size_t alloc_size_min = esize + PAGE_CEILING(alignment) - PAGE;
	/* Beware size_t wrap-around. */
	if (alloc_size_min < esize) {
		return NULL;
	}
	if (alloc_size < alloc_size_min) {
		return NULL;
	}
	extent_t *extent = extent_alloc(tsdn, arena);
	if (extent == NULL) {
		return NULL;
	}
	bool zeroed = false;
	bool committed = false;
	void *ptr = extent_alloc_core(tsdn, arena, new_addr, alloc_size, PAGE,
	    &zeroed, &committed, (dss_prec_t)atomic_load_u(&arena->dss_prec,
	    ATOMIC_RELAXED));
	extent_init(extent, arena, ptr, alloc_size, false, NSIZES,
	    arena_extent_sn_next(arena), extent_state_active, zeroed,
	    committed);
	if (ptr == NULL || extent_register_no_gdump_add(tsdn, extent)) {
		extent_dalloc(tsdn, arena, extent);
		return NULL;
	}

	size_t leadsize = ALIGNMENT_CEILING((uintptr_t)ptr,
	    PAGE_CEILING(alignment)) - (uintptr_t)ptr;
	assert(new_addr == NULL || leadsize == 0);
	assert(alloc_size >= leadsize + esize);
	size_t trailsize = alloc_size - leadsize - esize;
	if (extent_zeroed_get(extent)) {
		*zero = true;
	}
	if (extent_committed_get(extent)) {
		*commit = true;
	}

	/* Split the lead. */
	if (leadsize != 0) {
		extent_t *lead = extent;
		extent = extent_split_wrapper(tsdn, arena, r_extent_hooks, lead,
		    leadsize, NSIZES, false, esize + trailsize, szind, slab);
		if (extent == NULL) {
			extent_deregister(tsdn, lead);
			extents_leak(tsdn, arena, r_extent_hooks, false, lead);
			return NULL;
		}
		extent_record(tsdn, arena, r_extent_hooks,
		    &arena->extents_retained, lead);
	}

	/* Split the trail. */
	if (trailsize != 0) {
		extent_t *trail = extent_split_wrapper(tsdn, arena,
		    r_extent_hooks, extent, esize, szind, slab, trailsize,
		    NSIZES, false);
		if (trail == NULL) {
			extent_deregister(tsdn, extent);
			extents_leak(tsdn, arena, r_extent_hooks,
			    &arena->extents_retained, extent);
			return NULL;
		}
		extent_record(tsdn, arena, r_extent_hooks,
		    &arena->extents_retained, trail);
	} else if (leadsize == 0) {
		/*
		 * Splitting causes szind to be set as a side effect, but no
		 * splitting occurred.
		 */
		rtree_ctx_t rtree_ctx_fallback;
		rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn,
		    &rtree_ctx_fallback);

		extent_szind_set(extent, szind);
		if (szind != NSIZES) {
			rtree_szind_slab_update(tsdn, &extents_rtree, rtree_ctx,
			    (uintptr_t)extent_addr_get(extent), szind, slab);
			if (slab && extent_size_get(extent) > PAGE) {
				rtree_szind_slab_update(tsdn, &extents_rtree,
				    rtree_ctx,
				    (uintptr_t)extent_past_get(extent) -
				    (uintptr_t)PAGE, szind, slab);
			}
		}
	}

	if (*commit && !extent_committed_get(extent)) {
		if (extent_commit_wrapper(tsdn, arena, r_extent_hooks, extent,
		    0, extent_size_get(extent))) {
			extent_record(tsdn, arena, r_extent_hooks,
			    &arena->extents_retained, extent);
			return NULL;
		}
		extent_zeroed_set(extent, true);
	}

	if (config_prof) {
		/* Adjust gdump stats now that extent is final size. */
		extent_gdump_add(tsdn, extent);
	}
	if (pad != 0) {
		extent_addr_randomize(tsdn, extent, alignment);
	}
	if (slab) {
		rtree_ctx_t rtree_ctx_fallback;
		rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn,
		    &rtree_ctx_fallback);

		extent_slab_set(extent, true);
		extent_interior_register(tsdn, rtree_ctx, extent, szind);
	}
	if (*zero && !extent_zeroed_get(extent)) {
		void *addr = extent_base_get(extent);
		size_t size = extent_size_get(extent);
		if (pages_purge_forced(addr, size)) {
			memset(addr, 0, size);
		}
	}
	/*
	 * Increment extent_grow_next, but take care to do so atomically and
	 * bail out if the increment would exceed the legal range.
	 */
	pszind_t egn = atomic_load_u(&arena->extent_grow_next, ATOMIC_RELAXED);
	while (true) {
		if (egn + 1 == NPSIZES) {
			break;
		}
		assert(egn + 1 < NPSIZES);
		if (atomic_compare_exchange_weak_u(&arena->extent_grow_next,
		    &egn, egn + 1, ATOMIC_RELAXED, ATOMIC_RELAXED)) {
			break;
		}
	}
	return extent;
}

static extent_t *
extent_alloc_retained(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, void *new_addr, size_t size, size_t pad,
    size_t alignment, bool slab, szind_t szind, bool *zero, bool *commit) {
	extent_t *extent;

	assert(size != 0);
	assert(alignment != 0);

	extent = extent_recycle(tsdn, arena, r_extent_hooks,
	    &arena->extents_retained, new_addr, size, pad, alignment, slab,
	    szind, zero, commit);
	if (extent != NULL) {
		if (config_prof) {
			extent_gdump_add(tsdn, extent);
		}
	}
	if (!config_munmap && extent == NULL) {
		extent = extent_grow_retained(tsdn, arena, r_extent_hooks,
		    new_addr, size, pad, alignment, slab, szind, zero, commit);
	}

	return extent;
}

static extent_t *
extent_alloc_wrapper_hard(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, void *new_addr, size_t size, size_t pad,
    size_t alignment, bool slab, szind_t szind, bool *zero, bool *commit) {
	size_t esize = size + pad;
	extent_t *extent = extent_alloc(tsdn, arena);
	if (extent == NULL) {
		return NULL;
	}
	void *addr;
	if (*r_extent_hooks == &extent_hooks_default) {
		/* Call directly to propagate tsdn. */
		addr = extent_alloc_default_impl(tsdn, arena, new_addr, esize,
		    alignment, zero, commit);
	} else {
		addr = (*r_extent_hooks)->alloc(*r_extent_hooks, new_addr,
		    esize, alignment, zero, commit, arena_ind_get(arena));
	}
	if (addr == NULL) {
		extent_dalloc(tsdn, arena, extent);
		return NULL;
	}
	extent_init(extent, arena, addr, esize, slab, szind,
	    arena_extent_sn_next(arena), extent_state_active, zero, commit);
	if (pad != 0) {
		extent_addr_randomize(tsdn, extent, alignment);
	}
	if (extent_register(tsdn, extent)) {
		extents_leak(tsdn, arena, r_extent_hooks,
		    &arena->extents_retained, extent);
		return NULL;
	}

	return extent;
}

extent_t *
extent_alloc_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, void *new_addr, size_t size, size_t pad,
    size_t alignment, bool slab, szind_t szind, bool *zero, bool *commit) {
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	extent_hooks_assure_initialized(arena, r_extent_hooks);

	extent_t *extent = extent_alloc_retained(tsdn, arena, r_extent_hooks,
	    new_addr, size, pad, alignment, slab, szind, zero, commit);
	if (extent == NULL) {
		extent = extent_alloc_wrapper_hard(tsdn, arena, r_extent_hooks,
		    new_addr, size, pad, alignment, slab, szind, zero, commit);
	}

	return extent;
}

static bool
extent_can_coalesce(arena_t *arena, extents_t *extents, const extent_t *inner,
    const extent_t *outer) {
	assert(extent_arena_get(inner) == arena);
	if (extent_arena_get(outer) != arena) {
		return false;
	}

	assert(extent_state_get(inner) == extent_state_active);
	if (extent_state_get(outer) != extents->state) {
		return false;
	}

	if (extent_committed_get(inner) != extent_committed_get(outer)) {
		return false;
	}

	return true;
}

static bool
extent_coalesce(tsdn_t *tsdn, arena_t *arena, extent_hooks_t **r_extent_hooks,
    extents_t *extents, extent_t *inner, extent_t *outer, bool forward) {
	assert(extent_can_coalesce(arena, extents, inner, outer));

	if (forward && extents->delay_coalesce) {
		/*
		 * The extent that remains after coalescing must occupy the
		 * outer extent's position in the LRU.  For forward coalescing,
		 * swap the inner extent into the LRU.
		 */
		extent_list_replace(&extents->lru, outer, inner);
	}
	extent_activate_locked(tsdn, arena, extents, outer,
	    extents->delay_coalesce);

	malloc_mutex_unlock(tsdn, &extents->mtx);
	bool err = extent_merge_wrapper(tsdn, arena, r_extent_hooks,
	    forward ? inner : outer, forward ? outer : inner);
	malloc_mutex_lock(tsdn, &extents->mtx);

	if (err) {
		if (forward && extents->delay_coalesce) {
			extent_list_replace(&extents->lru, inner, outer);
		}
		extent_deactivate_locked(tsdn, arena, extents, outer,
		    extents->delay_coalesce);
	}

	return err;
}

static extent_t *
extent_try_coalesce(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, rtree_ctx_t *rtree_ctx, extents_t *extents,
    extent_t *extent, bool *coalesced) {
	/*
	 * Continue attempting to coalesce until failure, to protect against
	 * races with other threads that are thwarted by this one.
	 */
	bool again;
	do {
		again = false;

		/* Try to coalesce forward. */
		rtree_leaf_elm_t *next_elm = rtree_leaf_elm_acquire(tsdn,
		    &extents_rtree, rtree_ctx,
		    (uintptr_t)extent_past_get(extent), false, false);
		if (next_elm != NULL) {
			extent_t *next = rtree_leaf_elm_extent_read(tsdn,
			    &extents_rtree, next_elm, true, true);
			/*
			 * extents->mtx only protects against races for
			 * like-state extents, so call extent_can_coalesce()
			 * before releasing the next_elm lock.
			 */
			bool can_coalesce = (next != NULL &&
			    extent_can_coalesce(arena, extents, extent, next));
			rtree_leaf_elm_release(tsdn, &extents_rtree, next_elm);
			if (can_coalesce && !extent_coalesce(tsdn, arena,
			    r_extent_hooks, extents, extent, next, true)) {
				if (extents->delay_coalesce) {
					/* Do minimal coalescing. */
					*coalesced = true;
					return extent;
				}
				again = true;
			}
		}

		/* Try to coalesce backward. */
		rtree_leaf_elm_t *prev_elm = rtree_leaf_elm_acquire(tsdn,
		    &extents_rtree, rtree_ctx,
		    (uintptr_t)extent_before_get(extent), false, false);
		if (prev_elm != NULL) {
			extent_t *prev = rtree_leaf_elm_extent_read(tsdn,
			    &extents_rtree, prev_elm, true, true);
			bool can_coalesce = (prev != NULL &&
			    extent_can_coalesce(arena, extents, extent, prev));
			rtree_leaf_elm_release(tsdn, &extents_rtree, prev_elm);
			if (can_coalesce && !extent_coalesce(tsdn, arena,
			    r_extent_hooks, extents, extent, prev, false)) {
				extent = prev;
				if (extents->delay_coalesce) {
					/* Do minimal coalescing. */
					*coalesced = true;
					return extent;
				}
				again = true;
			}
		}
	} while (again);

	if (extents->delay_coalesce) {
		*coalesced = false;
	}
	return extent;
}

static void
extent_record(tsdn_t *tsdn, arena_t *arena, extent_hooks_t **r_extent_hooks,
    extents_t *extents, extent_t *extent) {
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

	assert((extents_state_get(extents) != extent_state_dirty &&
	    extents_state_get(extents) != extent_state_muzzy) ||
	    !extent_zeroed_get(extent));

	malloc_mutex_lock(tsdn, &extents->mtx);
	extent_hooks_assure_initialized(arena, r_extent_hooks);

	extent_szind_set(extent, NSIZES);
	if (extent_slab_get(extent)) {
		extent_interior_deregister(tsdn, rtree_ctx, extent);
		extent_slab_set(extent, false);
	}

	assert(rtree_extent_read(tsdn, &extents_rtree, rtree_ctx,
	    (uintptr_t)extent_base_get(extent), true) == extent);

	if (!extents->delay_coalesce) {
		extent = extent_try_coalesce(tsdn, arena, r_extent_hooks,
		    rtree_ctx, extents, extent, NULL);
	}

	extent_deactivate_locked(tsdn, arena, extents, extent, false);

	malloc_mutex_unlock(tsdn, &extents->mtx);
}

void
extent_dalloc_gap(tsdn_t *tsdn, arena_t *arena, extent_t *extent) {
	extent_hooks_t *extent_hooks = EXTENT_HOOKS_INITIALIZER;

	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	if (extent_register(tsdn, extent)) {
		extents_leak(tsdn, arena, &extent_hooks,
		    &arena->extents_retained, extent);
		return;
	}
	extent_dalloc_wrapper(tsdn, arena, &extent_hooks, extent);
}

static bool
extent_dalloc_default_impl(void *addr, size_t size) {
	if (!have_dss || !extent_in_dss(addr)) {
		return extent_dalloc_mmap(addr, size);
	}
	return true;
}

static bool
extent_dalloc_default(extent_hooks_t *extent_hooks, void *addr, size_t size,
    bool committed, unsigned arena_ind) {
	assert(extent_hooks == &extent_hooks_default);

	return extent_dalloc_default_impl(addr, size);
}

bool
extent_dalloc_wrapper_try(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent) {
	bool err;

	assert(extent_base_get(extent) != NULL);
	assert(extent_size_get(extent) != 0);
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	extent_addr_set(extent, extent_base_get(extent));

	extent_hooks_assure_initialized(arena, r_extent_hooks);
	/* Try to deallocate. */
	if (*r_extent_hooks == &extent_hooks_default) {
		/* Call directly to propagate tsdn. */
		err = extent_dalloc_default_impl(extent_base_get(extent),
		    extent_size_get(extent));
	} else {
		err = ((*r_extent_hooks)->dalloc == NULL ||
		    (*r_extent_hooks)->dalloc(*r_extent_hooks,
		    extent_base_get(extent), extent_size_get(extent),
		    extent_committed_get(extent), arena_ind_get(arena)));
	}

	if (!err) {
		extent_dalloc(tsdn, arena, extent);
	}

	return err;
}

void
extent_dalloc_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent) {
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	/*
	 * Deregister first to avoid a race with other allocating threads, and
	 * reregister if deallocation fails.
	 */
	extent_deregister(tsdn, extent);
	if (!extent_dalloc_wrapper_try(tsdn, arena, r_extent_hooks, extent)) {
		return;
	}

	extent_reregister(tsdn, extent);
	/* Try to decommit; purge if that fails. */
	bool zeroed;
	if (!extent_committed_get(extent)) {
		zeroed = true;
	} else if (!extent_decommit_wrapper(tsdn, arena, r_extent_hooks, extent,
	    0, extent_size_get(extent))) {
		zeroed = true;
	} else if ((*r_extent_hooks)->purge_forced != NULL &&
	    !(*r_extent_hooks)->purge_forced(*r_extent_hooks,
	    extent_base_get(extent), extent_size_get(extent), 0,
	    extent_size_get(extent), arena_ind_get(arena))) {
		zeroed = true;
	} else if (extent_state_get(extent) == extent_state_muzzy ||
	    ((*r_extent_hooks)->purge_lazy != NULL &&
	    !(*r_extent_hooks)->purge_lazy(*r_extent_hooks,
	    extent_base_get(extent), extent_size_get(extent), 0,
	    extent_size_get(extent), arena_ind_get(arena)))) {
		zeroed = false;
	} else {
		zeroed = false;
	}
	extent_zeroed_set(extent, zeroed);

	if (config_prof) {
		extent_gdump_sub(tsdn, extent);
	}

	extent_record(tsdn, arena, r_extent_hooks, &arena->extents_retained,
	    extent);
}

static bool
extent_commit_default(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	assert(extent_hooks == &extent_hooks_default);

	return pages_commit((void *)((uintptr_t)addr + (uintptr_t)offset),
	    length);
}

bool
extent_commit_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent, size_t offset,
    size_t length) {
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	extent_hooks_assure_initialized(arena, r_extent_hooks);
	bool err = ((*r_extent_hooks)->commit == NULL ||
	    (*r_extent_hooks)->commit(*r_extent_hooks, extent_base_get(extent),
	    extent_size_get(extent), offset, length, arena_ind_get(arena)));
	extent_committed_set(extent, extent_committed_get(extent) || !err);
	return err;
}

static bool
extent_decommit_default(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	assert(extent_hooks == &extent_hooks_default);

	return pages_decommit((void *)((uintptr_t)addr + (uintptr_t)offset),
	    length);
}

bool
extent_decommit_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent, size_t offset,
    size_t length) {
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	extent_hooks_assure_initialized(arena, r_extent_hooks);

	bool err = ((*r_extent_hooks)->decommit == NULL ||
	    (*r_extent_hooks)->decommit(*r_extent_hooks,
	    extent_base_get(extent), extent_size_get(extent), offset, length,
	    arena_ind_get(arena)));
	extent_committed_set(extent, extent_committed_get(extent) && err);
	return err;
}

#ifdef PAGES_CAN_PURGE_LAZY
static bool
extent_purge_lazy_default(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	assert(extent_hooks == &extent_hooks_default);
	assert(addr != NULL);
	assert((offset & PAGE_MASK) == 0);
	assert(length != 0);
	assert((length & PAGE_MASK) == 0);

	return pages_purge_lazy((void *)((uintptr_t)addr + (uintptr_t)offset),
	    length);
}
#endif

bool
extent_purge_lazy_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent, size_t offset,
    size_t length) {
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	extent_hooks_assure_initialized(arena, r_extent_hooks);
	return ((*r_extent_hooks)->purge_lazy == NULL ||
	    (*r_extent_hooks)->purge_lazy(*r_extent_hooks,
	    extent_base_get(extent), extent_size_get(extent), offset, length,
	    arena_ind_get(arena)));
}

#ifdef PAGES_CAN_PURGE_FORCED
static bool
extent_purge_forced_default(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t offset, size_t length, unsigned arena_ind) {
	assert(extent_hooks == &extent_hooks_default);
	assert(addr != NULL);
	assert((offset & PAGE_MASK) == 0);
	assert(length != 0);
	assert((length & PAGE_MASK) == 0);

	return pages_purge_forced((void *)((uintptr_t)addr +
	    (uintptr_t)offset), length);
}
#endif

bool
extent_purge_forced_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent, size_t offset,
    size_t length) {
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	extent_hooks_assure_initialized(arena, r_extent_hooks);
	return ((*r_extent_hooks)->purge_forced == NULL ||
	    (*r_extent_hooks)->purge_forced(*r_extent_hooks,
	    extent_base_get(extent), extent_size_get(extent), offset, length,
	    arena_ind_get(arena)));
}

#ifdef JEMALLOC_MAPS_COALESCE
static bool
extent_split_default(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t size_a, size_t size_b, bool committed, unsigned arena_ind) {
	assert(extent_hooks == &extent_hooks_default);

	return !maps_coalesce;
}
#endif

extent_t *
extent_split_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent, size_t size_a,
    szind_t szind_a, bool slab_a, size_t size_b, szind_t szind_b, bool slab_b) {
	assert(extent_size_get(extent) == size_a + size_b);
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	extent_t *trail;
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	rtree_leaf_elm_t *lead_elm_a, *lead_elm_b, *trail_elm_a, *trail_elm_b;

	extent_hooks_assure_initialized(arena, r_extent_hooks);

	if ((*r_extent_hooks)->split == NULL) {
		return NULL;
	}

	trail = extent_alloc(tsdn, arena);
	if (trail == NULL) {
		goto label_error_a;
	}

	{
		extent_t lead;

		extent_init(&lead, arena, extent_addr_get(extent), size_a,
		    slab_a, szind_a, extent_sn_get(extent),
		    extent_state_get(extent), extent_zeroed_get(extent),
		    extent_committed_get(extent));

		if (extent_rtree_acquire(tsdn, rtree_ctx, &lead, false, true,
		    &lead_elm_a, &lead_elm_b)) {
			goto label_error_b;
		}
	}

	extent_init(trail, arena, (void *)((uintptr_t)extent_base_get(extent) +
	    size_a), size_b, slab_b, szind_b, extent_sn_get(extent),
	    extent_state_get(extent), extent_zeroed_get(extent),
	    extent_committed_get(extent));
	if (extent_rtree_acquire(tsdn, rtree_ctx, trail, false, true,
	    &trail_elm_a, &trail_elm_b)) {
		goto label_error_c;
	}

	if ((*r_extent_hooks)->split(*r_extent_hooks, extent_base_get(extent),
	    size_a + size_b, size_a, size_b, extent_committed_get(extent),
	    arena_ind_get(arena))) {
		goto label_error_d;
	}

	extent_size_set(extent, size_a);
	extent_szind_set(extent, szind_a);

	extent_rtree_write_acquired(tsdn, lead_elm_a, lead_elm_b, extent,
	    szind_a, slab_a);
	extent_rtree_write_acquired(tsdn, trail_elm_a, trail_elm_b, trail,
	    szind_b, slab_b);

	extent_rtree_release(tsdn, lead_elm_a, lead_elm_b);
	extent_rtree_release(tsdn, trail_elm_a, trail_elm_b);

	return trail;
label_error_d:
	extent_rtree_release(tsdn, trail_elm_a, trail_elm_b);
label_error_c:
	extent_rtree_release(tsdn, lead_elm_a, lead_elm_b);
label_error_b:
	extent_dalloc(tsdn, arena, trail);
label_error_a:
	return NULL;
}

static bool
extent_merge_default_impl(void *addr_a, void *addr_b) {
	if (!maps_coalesce) {
		return true;
	}
	if (have_dss && !extent_dss_mergeable(addr_a, addr_b)) {
		return true;
	}

	return false;
}

#ifdef JEMALLOC_MAPS_COALESCE
static bool
extent_merge_default(extent_hooks_t *extent_hooks, void *addr_a, size_t size_a,
    void *addr_b, size_t size_b, bool committed, unsigned arena_ind) {
	assert(extent_hooks == &extent_hooks_default);

	return extent_merge_default_impl(addr_a, addr_b);
}
#endif

bool
extent_merge_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *a, extent_t *b) {
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	extent_hooks_assure_initialized(arena, r_extent_hooks);

	if ((*r_extent_hooks)->merge == NULL) {
		return true;
	}

	bool err;
	if (*r_extent_hooks == &extent_hooks_default) {
		/* Call directly to propagate tsdn. */
		err = extent_merge_default_impl(extent_base_get(a),
		    extent_base_get(b));
	} else {
		err = (*r_extent_hooks)->merge(*r_extent_hooks,
		    extent_base_get(a), extent_size_get(a), extent_base_get(b),
		    extent_size_get(b), extent_committed_get(a),
		    arena_ind_get(arena));
	}

	if (err) {
		return true;
	}

	/*
	 * The rtree writes must happen while all the relevant elements are
	 * owned, so the following code uses decomposed helper functions rather
	 * than extent_{,de}register() to do things in the right order.
	 */
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	rtree_leaf_elm_t *a_elm_a, *a_elm_b, *b_elm_a, *b_elm_b;
	extent_rtree_acquire(tsdn, rtree_ctx, a, true, false, &a_elm_a,
	    &a_elm_b);
	extent_rtree_acquire(tsdn, rtree_ctx, b, true, false, &b_elm_a,
	    &b_elm_b);

	if (a_elm_b != NULL) {
		rtree_leaf_elm_write(tsdn, &extents_rtree, a_elm_b, true, NULL,
		    NSIZES, false);
		rtree_leaf_elm_release(tsdn, &extents_rtree, a_elm_b);
	}
	if (b_elm_b != NULL) {
		rtree_leaf_elm_write(tsdn, &extents_rtree, b_elm_a, true, NULL,
		    NSIZES, false);
		rtree_leaf_elm_release(tsdn, &extents_rtree, b_elm_a);
	} else {
		b_elm_b = b_elm_a;
	}

	extent_size_set(a, extent_size_get(a) + extent_size_get(b));
	extent_szind_set(a, NSIZES);
	extent_sn_set(a, (extent_sn_get(a) < extent_sn_get(b)) ?
	    extent_sn_get(a) : extent_sn_get(b));
	extent_zeroed_set(a, extent_zeroed_get(a) && extent_zeroed_get(b));

	extent_rtree_write_acquired(tsdn, a_elm_a, b_elm_b, a, NSIZES, false);
	extent_rtree_release(tsdn, a_elm_a, b_elm_b);

	extent_dalloc(tsdn, extent_arena_get(b), b);

	return false;
}

bool
extent_boot(void) {
	if (rtree_new(&extents_rtree, true)) {
		return true;
	}

	if (have_dss) {
		extent_dss_boot();
	}

	return false;
}
