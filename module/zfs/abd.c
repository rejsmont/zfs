/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2014 by Chunwei Chen. All rights reserved.
 * Copyright (c) 2016 by Delphix. All rights reserved.
 * Copyright (c) 2017 by Sean Doran. All rights reserved.
 */

/*
 * ARC buffer data (ABD).
 *
 * ABDs are an abstract data structure for the ARC which can use two
 * different ways of storing the underlying data:
 *
 * (a) Linear buffer. In this case, all the data in the ABD is stored in one
 *     contiguous buffer in memory (from a zio_[data_]buf_* kmem cache).
 *
 *         +-------------------+
 *         | ABD (linear)      |
 *         |   abd_flags = ... |
 *         |   abd_size = ...  |     +--------------------------------+
 *         |   abd_buf ------------->| raw buffer of size abd_size    |
 *         +-------------------+     +--------------------------------+
 *              no abd_chunks
 *
 * (b) Scattered buffer. In this case, the data in the ABD is split into
 *     equal-sized chunks (from the abd_chunk_cache kmem_cache), with pointers
 *     to the chunks recorded in an array at the end of the ABD structure.
 *
 *         +-------------------+
 *         | ABD (scattered)   |
 *         |   abd_flags = ... |
 *         |   abd_size = ...  |
 *         |   abd_offset = 0  |                           +-----------+
 *         |   abd_chunks[0] ----------------------------->| chunk 0   |
 *         |   abd_chunks[1] ---------------------+        +-----------+
 *         |   ...             |                  |        +-----------+
 *         |   abd_chunks[N-1] ---------+         +------->| chunk 1   |
 *         +-------------------+        |                  +-----------+
 *                                      |                      ...
 *                                      |                  +-----------+
 *                                      +----------------->| chunk N-1 |
 *                                                         +-----------+
 *
 * Using a large proportion of scattered ABDs decreases ARC fragmentation since
 * when we are at the limit of allocatable space, using equal-size chunks will
 * allow us to quickly reclaim enough space for a new large allocation (assuming
 * it is also scattered).
 *
 * In addition to directly allocating a linear or scattered ABD, it is also
 * possible to create an ABD by requesting the "sub-ABD" starting at an offset
 * within an existing ABD. In linear buffers this is simple (set abd_buf of
 * the new ABD to the starting point within the original raw buffer), but
 * scattered ABDs are a little more complex. The new ABD makes a copy of the
 * relevant abd_chunks pointers (but not the underlying data). However, to
 * provide arbitrary rather than only chunk-aligned starting offsets, it also
 * tracks an abd_offset field which represents the starting point of the data
 * within the first chunk in abd_chunks. For both linear and scattered ABDs,
 * creating an offset ABD marks the original ABD as the offset's parent, and the
 * original ABD's abd_children refcount is incremented. This data allows us to
 * ensure the root ABD isn't deleted before its children.
 *
 * Most consumers should never need to know what type of ABD they're using --
 * the ABD public API ensures that it's possible to transparently switch from
 * using a linear ABD to a scattered one when doing so would be beneficial.
 *
 * If you need to use the data within an ABD directly, if you know it's linear
 * (because you allocated it) you can use abd_to_buf() to access the underlying
 * raw buffer. Otherwise, you should use one of the abd_borrow_buf* functions
 * which will allocate a raw buffer if necessary. Use the abd_return_buf*
 * functions to return any raw buffers that are no longer necessary when you're
 * done using them.
 *
 * There are a variety of ABD APIs that implement basic buffer operations:
 * compare, copy, read, write, and fill with zeroes. If you need a custom
 * function which progressively accesses the whole ABD, use the abd_iterate_*
 * functions.
 */

#include <sys/abd.h>
#include <sys/param.h>
#include <sys/zio.h>
#include <sys/zfs_context.h>
#include <sys/zfs_znode.h>
#include <sys/debug.h>

#ifdef DEBUG
#ifdef _KERNEL
#define VERIFY_ABD_MAGIC(x) do {	      \
		const uint64_t y = (x)->abd_magic;	\
		if (y != ABD_DEBUG_MAGIC) {		\
			panic("VERIFY_ABD_MAGIC(" #x ") failed (0x%llx != 0x%llx )\n", \
			    y, ABD_DEBUG_MAGIC);			\
		}							\
	} while (0)
#else
#define VERIFY_ABD_MAGIC(x) do {	     \
	  const uint64_t y = (x)->abd_magic; \
	  if (y != ABD_DEBUG_MAGIC) {		     \
		  char * __buf = alloca(256);				\
		  (void) snprintf(__buf, 256,				\
		      "VERIFY_ABD_MAGIC(%s) failed (0x%llx != 0x%llx)", \
		      #x, y, ABD_DEBUG_MAGIC);				\
		  __assert_c99(__buf, __FILE__, __LINE__, __func__);	\
	  }								\
	} while (0)
#endif
#else
#define VERIFY_ABD_MAGIC(x)
#endif

#ifdef DEBUG
#ifdef _KERNEL
#define VERIFY_BUF_NOMAGIC(x, size) do {				\
		const uint64_t m = ((abd_t *)(x))->abd_magic;		\
		if ((size) >= sizeof(abd_t) && m == ABD_DEBUG_MAGIC) {	\
			panic("VERIFY_BUF_NOMAGIC(" #x ", 0x%lx) failed\n", size); \
		}							\
	} while (0)
#else
#define VERIFY_BUF_NOMAGIC(x, size) do {				\
		const uint64_t m = ((abd_t *)(x))->abd_magic;		\
		if ((size) >= sizeof(abd_t) && m == ABD_DEBUG_MAGIC) {	\
			char *__buf = alloca(256);			\
			(void) snprintf(__buf, 256,			\
			    "VERIFY_BUF_NOMAGIC(%s, 0x%lx)) failed", #x, size); \
			__assert_c99(__buf, __FILE__, __LINE__, __func__); \
		}							\
	} while (0)
#endif
#else
#define VERIFY_BUF_NOMAGIC(x, s)
#endif

typedef struct abd_stats {
	kstat_named_t abdstat_struct_size;
	kstat_named_t abdstat_scatter_cnt;
	kstat_named_t abdstat_scatter_data_size;
	kstat_named_t abdstat_scatter_chunk_waste;
	kstat_named_t abdstat_linear_cnt;
	kstat_named_t abdstat_linear_data_size;
	kstat_named_t abdstat_is_file_data_scattered;
	kstat_named_t abdstat_is_metadata_scattered;
	kstat_named_t abdstat_is_file_data_linear;
	kstat_named_t abdstat_is_metadata_linear;
	kstat_named_t abdstat_small_scatter_cnt;
	kstat_named_t abdstat_scattered_metadata_cnt;
	kstat_named_t abdstat_scattered_filedata_cnt;
	kstat_named_t abdstat_borrowed_buf_cnt;
	kstat_named_t abdstat_move_refcount_nonzero;
	kstat_named_t abdstat_moved_linear;
	kstat_named_t abdstat_moved_scattered_filedata;
	kstat_named_t abdstat_moved_scattered_metadata;
	kstat_named_t abdstat_move_to_buf_flag_fail;
} abd_stats_t;

static abd_stats_t abd_stats = {
	/* Amount of memory occupied by all of the abd_t struct allocations */
	{ "struct_size",			KSTAT_DATA_UINT64 },
	/*
	 * The number of scatter ABDs which are currently allocated, excluding
	 * ABDs which don't own their data (for instance the ones which were
	 * allocated through abd_get_offset()).
	 */
	{ "scatter_cnt",			KSTAT_DATA_UINT64 },
	/* Amount of data stored in all scatter ABDs tracked by scatter_cnt */
	{ "scatter_data_size",			KSTAT_DATA_UINT64 },
	/*
	 * The amount of space wasted at the end of the last chunk across all
	 * scatter ABDs tracked by scatter_cnt.
	 */
	{ "scatter_chunk_waste",		KSTAT_DATA_UINT64 },
	/*
	 * The number of linear ABDs which are currently allocated, excluding
	 * ABDs which don't own their data (for instance the ones which were
	 * allocated through abd_get_offset() and abd_get_from_buf()). If an
	 * ABD takes ownership of its buf then it will become tracked.
	 */
	{ "linear_cnt",				KSTAT_DATA_UINT64 },
	/* Amount of data stored in all linear ABDs tracked by linear_cnt */
	{ "linear_data_size",			KSTAT_DATA_UINT64 },
	/* Amount of data that is respectively file data and metadata */
	{ "is_file_data_scattered",             KSTAT_DATA_UINT64 },
	{ "is_metadata_scattered",              KSTAT_DATA_UINT64 },
	{ "is_file_data_linear",                KSTAT_DATA_UINT64 },
	{ "is_metadata_linear",                 KSTAT_DATA_UINT64 },
	/* Number of allocations linearized because < zfs_abd_chunk_size */
	{ "small_scatter_cnt",                  KSTAT_DATA_UINT64 },
	/* Counts, respectively, of metadata buffers vs file data buffers */
	{ "metadata_scattered_buffers",         KSTAT_DATA_UINT64 },
	{ "filedata_scattered_buffers",         KSTAT_DATA_UINT64 },
	/* number of borrowed bufs */
	{ "borrowed_bufs",                      KSTAT_DATA_UINT64 },
	/* abd_try_move() statistics */
	{ "move_refcount_nonzero",              KSTAT_DATA_UINT64 },
	{ "moved_linear",                       KSTAT_DATA_UINT64 },
	{ "moved_scattered_filedata",           KSTAT_DATA_UINT64 },
	{ "moved_scattered_metadata",           KSTAT_DATA_UINT64 },
	{ "move_to_buf_flag_fail",              KSTAT_DATA_UINT64 },
};

#define	ABDSTAT(stat)		(abd_stats.stat.value.ui64)
#define	ABDSTAT_INCR(stat, val) \
	atomic_add_64(&abd_stats.stat.value.ui64, (val))
#define	ABDSTAT_BUMP(stat)	ABDSTAT_INCR(stat, 1)
#define	ABDSTAT_BUMPDOWN(stat)	ABDSTAT_INCR(stat, -1)

/*
 * It is possible to make all future ABDs be linear by setting this to B_FALSE.
 * Otherwise, ABDs are allocated scattered by default unless the caller uses
 * abd_alloc_linear().
 */
boolean_t zfs_abd_scatter_enabled = B_TRUE;

/*
 * The size of the chunks ABD allocates. Because the sizes allocated from the
 * kmem_cache can't change, this tunable can only be modified at boot. Changing
 * it at runtime would cause ABD iteration to work incorrectly for ABDs which
 * were allocated with the old size, so a safeguard has been put in place which
 * will cause the machine to panic if you change it and try to access the data
 * within a scattered ABD.
 */

size_t zfs_abd_chunk_size = 1024;

#ifdef _KERNEL
extern vmem_t *zio_arena;
#endif

kmem_cache_t *abd_chunk_cache;
static kstat_t *abd_ksp;

static void *
abd_alloc_chunk()
{
	void *c = kmem_cache_alloc(abd_chunk_cache, KM_PUSHPAGE);
	ASSERT3P(c, !=, NULL);
	return (c);
}

static void
abd_free_chunk(void *c)
{
	kmem_cache_free(abd_chunk_cache, c);
}

#ifdef __APPLE__
/* use this function during abd moving */
static void
abd_free_chunk_to_slab(void *c)
{
#ifdef _KERNEL
	kmem_cache_free_to_slab(abd_chunk_cache, c);
#else
	kmem_cache_free(abd_chunk_cache, c);
#endif
}
#endif


#if defined(__APPLE__) && defined(_KERNEL)
vmem_t *abd_chunk_arena = NULL;
#endif

void
abd_init(void)
{

#if !(defined(__APPLE__) && defined(_KERNEL))

	vmem_t *data_alloc_arena = NULL;

	/*
	 * Since ABD chunks do not appear in crash dumps, we pass KMC_NOTOUCH
	 * so that no allocator metadata is stored with the buffers.
	 */
	abd_chunk_cache = kmem_cache_create("abd_chunk", zfs_abd_chunk_size, 0,
	    NULL, NULL, NULL, NULL, data_alloc_arena, KMC_NOTOUCH);
#else
#define	KMF_AUDIT		0x00000001	/* transaction auditing */
#define KMF_DEADBEEF    0x00000002      /* deadbeef checking */
#define KMF_REDZONE             0x00000004      /* redzone checking */
#define KMF_CONTENTS    0x00000008      /* freed-buffer content logging */
#define KMF_LITE        0x00000100      /* lightweight debugging */
#define KMF_HASH                0x00000200      /* cache has hash table */
#define KMF_BUFTAG      (KMF_DEADBEEF | KMF_REDZONE)

	/* In xnu we crash dump differently anyway, so we can give
	 * a real alignment argument (instead of using 0 == KMEM_ALGN == 8)
	 * and also turn on debugging flags
	 */

	/* sanity check */
	VERIFY(ISP2(zfs_abd_chunk_size)); /* must be power of two */

	//extern vmem_t *zio_arena_parent;
	extern vmem_t *spl_heap_arena;

	abd_chunk_arena = vmem_create("abd_chunk", NULL, 0,
	    PAGESIZE, vmem_alloc, vmem_free, spl_heap_arena,
	    64*1024, VM_SLEEP | VMC_NO_QCACHE | VMC_TIMEFREE);

	ASSERT3P(abd_chunk_arena, !=, NULL);

	//int cache_debug_flags = KMF_BUFTAG | KMF_HASH | KMF_AUDIT;
	//int cache_debug_flags = KMF_BUFTAG | KMF_HASH | KMF_LITE;
	int cache_debug_flags = KMF_HASH | KMC_NOTOUCH;
	cache_debug_flags |= KMC_ARENA_SLAB; /* use large slabs */

	abd_chunk_cache = kmem_cache_create("abd_chunk", zfs_abd_chunk_size, zfs_abd_chunk_size,
	    NULL, NULL, NULL, NULL, abd_chunk_arena, cache_debug_flags);

	VERIFY3P(abd_chunk_cache, !=, NULL);
#endif

	abd_ksp = kstat_create("zfs", 0, "abdstats", "misc", KSTAT_TYPE_NAMED,
	    sizeof (abd_stats) / sizeof (kstat_named_t), KSTAT_FLAG_VIRTUAL);
	if (abd_ksp != NULL) {
		abd_ksp->ks_data = &abd_stats;
		kstat_install(abd_ksp);
	}
}

void
abd_fini(void)
{
	if (abd_ksp != NULL) {
		kstat_delete(abd_ksp);
		abd_ksp = NULL;
	}

	kmem_cache_destroy(abd_chunk_cache);
	abd_chunk_cache = NULL;
#if defined(__APPLE__) && defined (_KERNEL)
	vmem_destroy(abd_chunk_arena);
#endif
}

static inline size_t
abd_chunkcnt_for_bytes(size_t size)
{
	return (P2ROUNDUP(size, zfs_abd_chunk_size) / zfs_abd_chunk_size);
}

static inline size_t
abd_scatter_chunkcnt(abd_t *abd)
{
	ASSERT(!abd_is_linear(abd));
	return (abd_chunkcnt_for_bytes(
	    abd->abd_u.abd_scatter.abd_offset + abd->abd_size));
}

static inline void
abd_verify(abd_t *abd)
{
	VERIFY_ABD_MAGIC(abd);

	ASSERT3U(abd->abd_size, >, 0);
	ASSERT3U(abd->abd_size, <=, SPA_MAXBLOCKSIZE);
	ASSERT3U(abd->abd_flags, ==, abd->abd_flags & (ABD_FLAG_LINEAR |
	    ABD_FLAG_OWNER | ABD_FLAG_META | ABD_FLAG_SMALL | ABD_FLAG_NOMOVE));
	IMPLY(abd->abd_parent != NULL, !(abd->abd_flags & ABD_FLAG_OWNER));
	IMPLY(abd->abd_flags & ABD_FLAG_META, abd->abd_flags & ABD_FLAG_OWNER);
	if (abd_is_linear(abd)) {
		ASSERT3P(abd->abd_u.abd_linear.abd_buf, !=, NULL);
	} else {
		ASSERT3U(abd->abd_u.abd_scatter.abd_offset, <,
		    zfs_abd_chunk_size);
		size_t n = abd_scatter_chunkcnt(abd);
		for (int i = 0; i < n; i++) {
			ASSERT3P(
			    abd->abd_u.abd_scatter.abd_chunks[i], !=, NULL);
		}
	}
}

static inline abd_t *
abd_alloc_struct(size_t chunkcnt)
{
	size_t size = offsetof(abd_t, abd_u.abd_scatter.abd_chunks[chunkcnt]);
	abd_t *abd = kmem_zalloc(size, KM_PUSHPAGE);
	ASSERT3P(abd, !=, NULL);
	ABDSTAT_INCR(abdstat_struct_size, size);
#ifdef DEBUG
	abd->abd_magic = ABD_DEBUG_MAGIC;
#endif
	abd->abd_create_time = gethrtime();
	mutex_init(&abd->abd_mutex, NULL, MUTEX_DEFAULT, NULL);

	return (abd);
}

static inline void
abd_free_struct(abd_t *abd)
{
	mutex_enter(&abd->abd_mutex);
	size_t chunkcnt = abd_is_linear(abd) ? 0 : abd_scatter_chunkcnt(abd);
	int size = offsetof(abd_t, abd_u.abd_scatter.abd_chunks[chunkcnt]);
	VERIFY_ABD_MAGIC(abd);
#ifdef DEBUG
	abd->abd_magic = 0;
#endif
	// poison the memory to catch UAF;
	abd->abd_u.abd_scatter.abd_chunk_size = 0;
	abd->abd_create_time = 0;
	abd->abd_flags = 0;
	abd->abd_parent = NULL;
	abd->abd_size = 0;
	abd->abd_u.abd_linear.abd_buf = NULL;
	mutex_exit(&abd->abd_mutex);
	mutex_destroy(&abd->abd_mutex);
	kmem_free(abd, size);
	ABDSTAT_INCR(abdstat_struct_size, -size);
}

/*
 * Allocate an ABD, along with its own underlying data buffers. Use this if you
 * don't care whether the ABD is linear or not.
 */
abd_t *
abd_alloc(size_t size, boolean_t is_metadata)
{
	if (!zfs_abd_scatter_enabled)
		return (abd_alloc_linear(size, is_metadata));

	VERIFY3U(size, <=, SPA_MAXBLOCKSIZE);

	size_t n = abd_chunkcnt_for_bytes(size);
	abd_t *abd = abd_alloc_struct(n);

	abd->abd_flags = ABD_FLAG_OWNER;
	if (is_metadata) {
		abd->abd_flags |= ABD_FLAG_META;
	}
	abd->abd_size = size;
	abd->abd_parent = NULL;
	refcount_create(&abd->abd_children);

	abd->abd_u.abd_scatter.abd_offset = 0;
	abd->abd_u.abd_scatter.abd_chunk_size = zfs_abd_chunk_size;

	for (int i = 0; i < n; i++) {
		void *c = abd_alloc_chunk();
		ASSERT3P(c, !=, NULL);
		abd->abd_u.abd_scatter.abd_chunks[i] = c;
	}

	ABDSTAT_BUMP(abdstat_scatter_cnt);
	ABDSTAT_INCR(abdstat_scatter_data_size, size);
	ABDSTAT_INCR(abdstat_scatter_chunk_waste,
	    n * zfs_abd_chunk_size - size);

	if (is_metadata) {
		ABDSTAT_INCR(abdstat_is_metadata_scattered, size);
		ABDSTAT_BUMP(abdstat_scattered_metadata_cnt);
	} else {
		ABDSTAT_INCR(abdstat_is_file_data_scattered, size);
		ABDSTAT_BUMP(abdstat_scattered_filedata_cnt);
	}

	if (size < zfs_abd_chunk_size) {
		ABDSTAT_BUMP(abdstat_small_scatter_cnt);
		abd->abd_flags |= ABD_FLAG_SMALL;
	}

	return (abd);
}

static void
abd_free_scatter(abd_t *abd)
{
	mutex_enter(&abd->abd_mutex);
	size_t n = abd_scatter_chunkcnt(abd);
	for (int i = 0; i < n; i++) {
		abd_free_chunk(abd->abd_u.abd_scatter.abd_chunks[i]);
	}

	refcount_destroy(&abd->abd_children);
	ABDSTAT_BUMPDOWN(abdstat_scatter_cnt);
	ABDSTAT_INCR(abdstat_scatter_data_size, -(int)abd->abd_size);
	ABDSTAT_INCR(abdstat_scatter_chunk_waste,
	    abd->abd_size - n * zfs_abd_chunk_size);

	if ((abd->abd_flags & ABD_FLAG_SMALL) != 0)
		ABDSTAT_BUMPDOWN(abdstat_small_scatter_cnt);

	int64_t unsize = -(int64_t)abd->abd_size;
	boolean_t is_metadata = (abd->abd_flags & ABD_FLAG_META) != 0;
	if (is_metadata) {
		ABDSTAT_INCR(abdstat_is_metadata_scattered, unsize);
		ABDSTAT_BUMPDOWN(abdstat_scattered_metadata_cnt);
	} else {
		ABDSTAT_INCR(abdstat_is_file_data_scattered, unsize);
		ABDSTAT_BUMPDOWN(abdstat_scattered_filedata_cnt);
	}

	mutex_exit(&abd->abd_mutex);
	abd_free_struct(abd);
}

/*
 * Allocate an ABD that must be linear, along with its own underlying data
 * buffer. Only use this when it would be very annoying to write your ABD
 * consumer with a scattered ABD.
 */
abd_t *
abd_alloc_linear(size_t size, boolean_t is_metadata)
{
	abd_t *abd = abd_alloc_struct(0);

	VERIFY3U(size, <=, SPA_MAXBLOCKSIZE);

	abd->abd_flags = ABD_FLAG_LINEAR | ABD_FLAG_OWNER;
	if (is_metadata) {
		abd->abd_flags |= ABD_FLAG_META;
	}
	abd->abd_size = size;
	abd->abd_parent = NULL;
	refcount_create(&abd->abd_children);

	if (is_metadata) {
		abd->abd_u.abd_linear.abd_buf = zio_buf_alloc(size);
	} else {
		abd->abd_u.abd_linear.abd_buf = zio_data_buf_alloc(size);
	}

	ABDSTAT_BUMP(abdstat_linear_cnt);
	ABDSTAT_INCR(abdstat_linear_data_size, size);

	if (is_metadata) {
		ABDSTAT_INCR(abdstat_is_metadata_linear, size);
	} else {
		ABDSTAT_INCR(abdstat_is_file_data_linear, size);
	}

	return (abd);
}

static void
abd_free_linear(abd_t *abd)
{
	mutex_enter(&abd->abd_mutex);

	if (abd->abd_flags & ABD_FLAG_META) {
		zio_buf_free(abd->abd_u.abd_linear.abd_buf, abd->abd_size);
	} else {
		zio_data_buf_free(abd->abd_u.abd_linear.abd_buf, abd->abd_size);
	}

	refcount_destroy(&abd->abd_children);
	ABDSTAT_BUMPDOWN(abdstat_linear_cnt);
	ABDSTAT_INCR(abdstat_linear_data_size, -(int)abd->abd_size);

	int64_t unsize = -(int64_t)abd->abd_size;
	boolean_t is_metadata = (abd->abd_flags & ABD_FLAG_META) != 0;
	if (is_metadata) {
		ABDSTAT_INCR(abdstat_is_metadata_linear, unsize);
	} else {
		ABDSTAT_INCR(abdstat_is_file_data_linear, unsize);
	}

	mutex_exit(&abd->abd_mutex);

	abd_free_struct(abd);
}

/*
 * Free an ABD. Only use this on ABDs allocated with abd_alloc() or
 * abd_alloc_linear().
 */
void
abd_free(abd_t *abd)
{
	mutex_enter(&abd->abd_mutex);
	abd_verify(abd);
	abd->abd_flags |= ABD_FLAG_NOMOVE;
	mutex_exit(&abd->abd_mutex);
	ASSERT3P(abd->abd_parent, ==, NULL);
	ASSERT(abd->abd_flags & ABD_FLAG_OWNER);
	if (abd_is_linear(abd))
		abd_free_linear(abd);
	else
		abd_free_scatter(abd);
}

/*
 * Allocate an ABD of the same format (same metadata flag, same scatterize
 * setting) as another ABD.
 */
abd_t *
abd_alloc_sametype(abd_t *sabd, size_t size)
{
	VERIFY_ABD_MAGIC(sabd);

	boolean_t is_metadata = (sabd->abd_flags & ABD_FLAG_META) != 0;
	if (abd_is_linear(sabd)) {
		return (abd_alloc_linear(size, is_metadata));
	} else {
		return (abd_alloc(size, is_metadata));
	}
}

/*
 * If we're going to use this ABD for doing I/O using the block layer, the
 * consumer of the ABD data doesn't care if it's scattered or not, and we don't
 * plan to store this ABD in memory for a long period of time, we should
 * allocate the ABD type that requires the least data copying to do the I/O.
 *
 * Currently this is linear ABDs, however if ldi_strategy() can ever issue I/Os
 * using a scatter/gather list we should switch to that and replace this call
 * with vanilla abd_alloc().
 */
abd_t *
abd_alloc_for_io(size_t size, boolean_t is_metadata)
{
	return (abd_alloc(size, is_metadata));
}

/*
 * Allocate a new ABD to point to offset off of sabd. It shares the underlying
 * buffer data with sabd. Use abd_put() to free. sabd must not be freed while
 * any derived ABDs exist.
 */
static inline abd_t *
abd_get_offset_impl(abd_t *sabd, size_t off, size_t size)
{
	abd_t *abd;

	mutex_enter(&sabd->abd_mutex);
	abd_verify(sabd);
	sabd->abd_flags |= ABD_FLAG_NOMOVE;
	ASSERT3U(off, <=, (size_t)sabd->abd_size);

	if (abd_is_linear(sabd)) {
		abd = abd_alloc_struct(0);

		/*
		 * Even if this buf is filesystem metadata, we only track that
		 * if we own the underlying data buffer, which is not true in
		 * this case. Therefore, we don't ever use ABD_FLAG_META here.
		 */
		abd->abd_flags = ABD_FLAG_LINEAR;

		abd->abd_u.abd_linear.abd_buf =
		    (char *)sabd->abd_u.abd_linear.abd_buf + off;
	} else {
		size_t new_offset = sabd->abd_u.abd_scatter.abd_offset + off;
		size_t chunkcnt = abd_scatter_chunkcnt(sabd) -
		    (new_offset / zfs_abd_chunk_size);

		abd = abd_alloc_struct(chunkcnt);

		/*
		 * Even if this buf is filesystem metadata, we only track that
		 * if we own the underlying data buffer, which is not true in
		 * this case. Therefore, we don't ever use ABD_FLAG_META here.
		 */
		abd->abd_flags = 0;

		abd->abd_u.abd_scatter.abd_offset =
		    new_offset % zfs_abd_chunk_size;
		abd->abd_u.abd_scatter.abd_chunk_size = zfs_abd_chunk_size;

		/* Copy the scatterlist starting at the correct offset */
		(void) memcpy(&abd->abd_u.abd_scatter.abd_chunks,
		    &sabd->abd_u.abd_scatter.abd_chunks[new_offset /
		    zfs_abd_chunk_size],
		    chunkcnt * sizeof (void *));
	}

	abd->abd_size = sabd->abd_size - off;
	abd->abd_parent = sabd;
	abd->abd_flags |= ABD_FLAG_NOMOVE;
	refcount_create(&abd->abd_children);
	(void) refcount_add_many(&sabd->abd_children, abd->abd_size, abd);
	mutex_exit(&sabd->abd_mutex);

	return (abd);
}

abd_t *
abd_get_offset(abd_t *sabd, size_t off)
{
	VERIFY_ABD_MAGIC(sabd);

	size_t size = sabd->abd_size > off ? sabd->abd_size - off : 0;

	VERIFY3U(size, >, 0);

	return (abd_get_offset_impl(sabd, off, size));
}

abd_t *
abd_get_offset_size(abd_t *sabd, size_t off, size_t size)
{
	VERIFY_ABD_MAGIC(sabd);

	ASSERT3U(off + size, <=, sabd->abd_size);

	return (abd_get_offset_impl(sabd, off, size));
}


/*
 * Allocate a linear ABD structure for buf. You must free this with abd_put()
 * since the resulting ABD doesn't own its own buffer.
 */
abd_t *
abd_get_from_buf(void *buf, size_t size)
{
	abd_t *abd = abd_alloc_struct(0);

	VERIFY_BUF_NOMAGIC(buf, size);

	VERIFY3U(size, <=, SPA_MAXBLOCKSIZE);

	/*
	 * Even if this buf is filesystem metadata, we only track that if we
	 * own the underlying data buffer, which is not true in this case.
	 * Therefore, we don't ever use ABD_FLAG_META here.
	 */
	abd->abd_flags = ABD_FLAG_LINEAR | ABD_FLAG_NOMOVE;
	abd->abd_size = size;
	abd->abd_parent = NULL;
	refcount_create(&abd->abd_children);

	abd->abd_u.abd_linear.abd_buf = buf;

	return (abd);
}

/*
 * Free an ABD allocated from abd_get_offset() or abd_get_from_buf(). Will not
 * free the underlying scatterlist or buffer.
 */
void
abd_put(abd_t *abd)
{
	mutex_enter(&abd->abd_mutex);
	abd_verify(abd);
	ASSERT(!(abd->abd_flags & ABD_FLAG_OWNER));

	if (abd->abd_parent != NULL) {
		mutex_enter(&abd->abd_parent->abd_mutex);
		(void) refcount_remove_many(&abd->abd_parent->abd_children,
		    abd->abd_size, abd);
		if (refcount_is_zero(&abd->abd_parent->abd_children))
			abd->abd_parent->abd_flags &= ~(ABD_FLAG_NOMOVE);
		mutex_exit(&abd->abd_parent->abd_mutex);
	}

	refcount_destroy(&abd->abd_children);
	mutex_exit(&abd->abd_mutex);
	abd_free_struct(abd);
}

/*
 * Get the raw buffer associated with a linear ABD.
 */
void *
abd_to_buf(abd_t *abd)
{
	ASSERT(abd_is_linear(abd));
	mutex_enter(&abd->abd_mutex);
	abd_verify(abd);
	abd->abd_flags |= ABD_FLAG_NOMOVE;
	mutex_exit(&abd->abd_mutex);
	return (abd->abd_u.abd_linear.abd_buf);
}

/* to be used in ASSERTs and other places where we do
 * not want to set ABD_FLAG_NOMOVE
 */
void *
abd_to_buf_ephemeral(abd_t *abd)
{
	ASSERT(abd_is_linear(abd));
	mutex_enter(&abd->abd_mutex);
	abd_verify(abd);
	mutex_exit(&abd->abd_mutex);
	return (abd->abd_u.abd_linear.abd_buf);
}

/*
 * Borrow a raw buffer from an ABD without copying the contents of the ABD
 * into the buffer. If the ABD is scattered, this will allocate a raw buffer
 * whose contents are undefined. To copy over the existing data in the ABD, use
 * abd_borrow_buf_copy() instead.
 */
void *
abd_borrow_buf(abd_t *abd, size_t n)
{
	void *buf;
	mutex_enter(&abd->abd_mutex);
	abd_verify(abd);
	ASSERT3U((size_t)abd->abd_size, >=, n);
	if (abd_is_linear(abd)) {
		mutex_exit(&abd->abd_mutex);
		buf = abd_to_buf(abd);
		mutex_enter(&abd->abd_mutex);
	} else {
		buf = zio_buf_alloc(n);
	}
	(void) refcount_add_many(&abd->abd_children, n, buf);
	mutex_exit(&abd->abd_mutex);

	ABDSTAT_BUMP(abdstat_borrowed_buf_cnt);

	return (buf);
}

void *
abd_borrow_buf_copy(abd_t *abd, size_t n)
{
	void *buf = abd_borrow_buf(abd, n);
	if (!abd_is_linear(abd)) {
		abd_copy_to_buf(buf, abd, n);
	}
	return (buf);
}

/*
 * Return a borrowed raw buffer to an ABD. If the ABD is scattered, this will
 * not change the contents of the ABD and will ASSERT that you didn't modify
 * the buffer since it was borrowed. If you want any changes you made to buf to
 * be copied back to abd, use abd_return_buf_copy() instead.
 */
void
abd_return_buf(abd_t *abd, void *buf, size_t n)
{
	mutex_enter(&abd->abd_mutex);
	abd_verify(abd);
	VERIFY_BUF_NOMAGIC(buf, n);
	ASSERT3U((size_t)abd->abd_size, >=, n);
	if (abd_is_linear(abd)) {
		mutex_exit(&abd->abd_mutex);
		ASSERT3P(buf, ==, abd_to_buf(abd));
		mutex_enter(&abd->abd_mutex);
	} else {
		mutex_exit(&abd->abd_mutex);
		ASSERT0(abd_cmp_buf(abd, buf, n));
		mutex_enter(&abd->abd_mutex);
		zio_buf_free(buf, n);
	}
	(void) refcount_remove_many(&abd->abd_children, n, buf);
	mutex_exit(&abd->abd_mutex);
	ABDSTAT_BUMPDOWN(abdstat_borrowed_buf_cnt);
}

void
abd_return_buf_copy(abd_t *abd, void *buf, size_t n)
{
	VERIFY_ABD_MAGIC(abd);
	VERIFY_BUF_NOMAGIC(buf, n);

	if (!abd_is_linear(abd)) {
		abd_copy_from_buf(abd, buf, n);
	}
	abd_return_buf(abd, buf, n);
}

/*
 *  functions to allow returns of bufs that are smaller than the abd size :
 *  this avoids ASSERTs in abd_cmp and abd_copy_from_buf
 */

void
abd_return_buf_copy_off(abd_t *abd, void *buf, size_t off, size_t len, size_t n)
{
	VERIFY_ABD_MAGIC(abd);
	VERIFY_BUF_NOMAGIC(buf, n);

	if (!abd_is_linear(abd)) {
		ASSERT3S(abd->abd_size,>=,off+len);
		abd_copy_from_buf_off(abd, buf, off, len);
	}
	abd_return_buf_off(abd, buf, off, len, n);
}


void
abd_return_buf_off(abd_t *abd, void *buf, size_t off, size_t len, size_t n)
{
	mutex_enter(&abd->abd_mutex);
	abd_verify(abd);
	VERIFY_BUF_NOMAGIC(buf, n);
	ASSERT3U((size_t)abd->abd_size, >=, n);
	if (abd_is_linear(abd)) {
		mutex_exit(&abd->abd_mutex);
		ASSERT3P(buf, ==, abd_to_buf(abd));
		mutex_enter(&abd->abd_mutex);
	} else {
		mutex_exit(&abd->abd_mutex);
		ASSERT0(abd_cmp_buf_off(abd, buf, off, len));
		mutex_enter(&abd->abd_mutex);
		zio_buf_free(buf, n);
	}
	(void) refcount_remove_many(&abd->abd_children, n, buf);
	mutex_exit(&abd->abd_mutex);
	ABDSTAT_BUMPDOWN(abdstat_borrowed_buf_cnt);
}

/*
 * Give this ABD ownership of the buffer that it's storing. Can only be used on
 * linear ABDs which were allocated via abd_get_from_buf(), or ones allocated
 * with abd_alloc_linear() which subsequently released ownership of their buf
 * with abd_release_ownership_of_buf().
 */
void
abd_take_ownership_of_buf(abd_t *abd, boolean_t is_metadata)
{
	mutex_enter(&abd->abd_mutex);
	ASSERT(abd_is_linear(abd));
	ASSERT(!(abd->abd_flags & ABD_FLAG_OWNER));
	abd_verify(abd);

	abd->abd_flags |= ABD_FLAG_OWNER;
	if (is_metadata) {
		abd->abd_flags |= ABD_FLAG_META;
		ABDSTAT_INCR(abdstat_is_metadata_linear, abd->abd_size);
	} else {
		ABDSTAT_INCR(abdstat_is_file_data_linear, abd->abd_size);
	}

	ABDSTAT_BUMP(abdstat_linear_cnt);
	ABDSTAT_INCR(abdstat_linear_data_size, abd->abd_size);

	mutex_exit(&abd->abd_mutex);
}

void
abd_release_ownership_of_buf(abd_t *abd)
{
	mutex_enter(&abd->abd_mutex);
	ASSERT(abd_is_linear(abd));
	ASSERT(abd->abd_flags & ABD_FLAG_OWNER);
	abd_verify(abd);

	abd->abd_flags &= ~ABD_FLAG_OWNER;
	/* Disable this flag since we no longer own the data buffer */
	abd->abd_flags &= ~ABD_FLAG_META;

	ABDSTAT_BUMPDOWN(abdstat_linear_cnt);
	ABDSTAT_INCR(abdstat_linear_data_size, -(int)abd->abd_size);

	mutex_exit(&abd->abd_mutex);
}

struct abd_iter {
	abd_t		*iter_abd;	/* ABD being iterated through */
	size_t		iter_pos;	/* position (relative to abd_offset) */
	void		*iter_mapaddr;	/* addr corresponding to iter_pos */
	size_t		iter_mapsize;	/* length of data valid at mapaddr */
};

static inline size_t
abd_iter_scatter_chunk_offset(struct abd_iter *aiter)
{
	ASSERT(!abd_is_linear(aiter->iter_abd));
	return ((aiter->iter_abd->abd_u.abd_scatter.abd_offset +
	    aiter->iter_pos) % zfs_abd_chunk_size);
}

static inline size_t
abd_iter_scatter_chunk_index(struct abd_iter *aiter)
{
	ASSERT(!abd_is_linear(aiter->iter_abd));
	return ((aiter->iter_abd->abd_u.abd_scatter.abd_offset +
	    aiter->iter_pos) / zfs_abd_chunk_size);
}

/*
 * Initialize the abd_iter.
 */
static void
abd_iter_init(struct abd_iter *aiter, abd_t *abd)
{
	abd_verify(abd);
	aiter->iter_abd = abd;
	aiter->iter_pos = 0;
	aiter->iter_mapaddr = NULL;
	aiter->iter_mapsize = 0;
}

/*
 * Advance the iterator by a certain amount. Cannot be called when a chunk is
 * in use. This can be safely called when the aiter has already exhausted, in
 * which case this does nothing.
 */
static void
abd_iter_advance(struct abd_iter *aiter, size_t amount)
{
	ASSERT3P(aiter->iter_mapaddr, ==, NULL);
	ASSERT0(aiter->iter_mapsize);

	/* There's nothing left to advance to, so do nothing */
	if (aiter->iter_pos == aiter->iter_abd->abd_size)
		return;

	aiter->iter_pos += amount;
}

/*
 * Map the current chunk into aiter. This can be safely called when the aiter
 * has already exhausted, in which case this does nothing.
 */
static void
abd_iter_map(struct abd_iter *aiter)
{
	void *paddr;
	size_t offset = 0;

	ASSERT3P(aiter->iter_mapaddr, ==, NULL);
	ASSERT0(aiter->iter_mapsize);

	/* Panic if someone has changed zfs_abd_chunk_size */
	IMPLY(!abd_is_linear(aiter->iter_abd), zfs_abd_chunk_size ==
	    aiter->iter_abd->abd_u.abd_scatter.abd_chunk_size);

	/* There's nothing left to iterate over, so do nothing */
	if (aiter->iter_pos == aiter->iter_abd->abd_size)
		return;

	if (abd_is_linear(aiter->iter_abd)) {
		offset = aiter->iter_pos;
		aiter->iter_mapsize = aiter->iter_abd->abd_size - offset;
		paddr = aiter->iter_abd->abd_u.abd_linear.abd_buf;
	} else {
		size_t index = abd_iter_scatter_chunk_index(aiter);
		offset = abd_iter_scatter_chunk_offset(aiter);
		aiter->iter_mapsize = zfs_abd_chunk_size - offset;
		paddr = aiter->iter_abd->abd_u.abd_scatter.abd_chunks[index];
	}
	aiter->iter_mapaddr = (char *)paddr + offset;
}

/*
 * Unmap the current chunk from aiter. This can be safely called when the aiter
 * has already exhausted, in which case this does nothing.
 */
static void
abd_iter_unmap(struct abd_iter *aiter)
{
	/* There's nothing left to unmap, so do nothing */
	if (aiter->iter_pos == aiter->iter_abd->abd_size)
		return;

	ASSERT3P(aiter->iter_mapaddr, !=, NULL);
	ASSERT3U(aiter->iter_mapsize, >, 0);

	aiter->iter_mapaddr = NULL;
	aiter->iter_mapsize = 0;
}

int
abd_iterate_func(abd_t *abd, size_t off, size_t size,
    abd_iter_func_t *func, void *private)
{
	int ret = 0;
	struct abd_iter aiter;

	mutex_enter(&abd->abd_mutex);
	abd_verify(abd);
	ASSERT3U(off + size, <=, (size_t)abd->abd_size);

	abd_iter_init(&aiter, abd);
	abd_iter_advance(&aiter, off);

	while (size > 0) {
		abd_iter_map(&aiter);

		size_t len = MIN(aiter.iter_mapsize, size);
		ASSERT3U(len, >, 0);

		ret = func(aiter.iter_mapaddr, len, private);

		abd_iter_unmap(&aiter);

		if (ret != 0)
			break;

		size -= len;
		abd_iter_advance(&aiter, len);
	}

	mutex_exit(&abd->abd_mutex);
	return (ret);
}

struct buf_arg {
	void *arg_buf;
};

static int
abd_copy_to_buf_off_cb(void *buf, size_t size, void *private)
{
	struct buf_arg *ba_ptr = private;

	(void) memcpy(ba_ptr->arg_buf, buf, size);
	ba_ptr->arg_buf = (char *)ba_ptr->arg_buf + size;

	return (0);
}

/*
 * Copy abd to buf. (off is the offset in abd.)
 */
void
abd_copy_to_buf_off(void *buf, abd_t *abd, size_t off, size_t size)
{
	struct buf_arg ba_ptr = { buf };

	VERIFY3P(buf,!=,NULL);
	VERIFY_BUF_NOMAGIC(buf, off+size);
	VERIFY_ABD_MAGIC(abd);

	ASSERT3S(size, >=, 0);
	ASSERT3S(off, >=, 0);
	ASSERT3S((size_t)abd->abd_size, >=, off+size);
	ASSERT3S((size_t)abd->abd_size, >, 0);

	(void) abd_iterate_func(abd, off, size, abd_copy_to_buf_off_cb,
	    &ba_ptr);
}

static int
abd_cmp_buf_off_cb(void *buf, size_t size, void *private)
{
	int ret;
	struct buf_arg *ba_ptr = private;

	ret = memcmp(buf, ba_ptr->arg_buf, size);
	ba_ptr->arg_buf = (char *)ba_ptr->arg_buf + size;

	return (ret);
}

/*
 * Compare the contents of abd to buf. (off is the offset in abd.)
 */
int
abd_cmp_buf_off(abd_t *abd, const void *buf, size_t off, size_t size)
{
	struct buf_arg ba_ptr = { (void *) buf };

	VERIFY_BUF_NOMAGIC(buf, off+size);
	VERIFY_ABD_MAGIC(abd);

	ASSERT3S(size, >, 0);
	ASSERT3S(off, >=, 0);
	ASSERT3S((size_t)abd->abd_size, >=, off+size);
	ASSERT3S((size_t)abd->abd_size, >, 0);

	return (abd_iterate_func(abd, off, size, abd_cmp_buf_off_cb, &ba_ptr));
}

static int
abd_copy_from_buf_off_cb(void *buf, size_t size, void *private)
{
	struct buf_arg *ba_ptr = private;

	(void) memcpy(buf, ba_ptr->arg_buf, size);
	ba_ptr->arg_buf = (char *)ba_ptr->arg_buf + size;

	return (0);
}

/*
 * Copy from buf to abd. (off is the offset in abd.)
 */
void
abd_copy_from_buf_off(abd_t *abd, const void *buf, size_t off, size_t size)
{
	struct buf_arg ba_ptr = { (void *) buf };

	VERIFY3P(buf,!=,NULL);
	VERIFY_BUF_NOMAGIC(buf, off+size);
	VERIFY_ABD_MAGIC(abd);

	ASSERT3S(size, >, 0);
	ASSERT3S(off, >=, 0);
	ASSERT3S((size_t)abd->abd_size, >=, off+size);
	ASSERT3S((size_t)abd->abd_size, >, 0);

	(void) abd_iterate_func(abd, off, size, abd_copy_from_buf_off_cb,
	    &ba_ptr);
}

/*ARGSUSED*/
static int
abd_zero_off_cb(void *buf, size_t size, void *private)
{
	(void) memset(buf, 0, size);
	return (0);
}

/*
 * Zero out the abd from a particular offset to the end.
 */
void
abd_zero_off(abd_t *abd, size_t off, size_t size)
{
	VERIFY_ABD_MAGIC(abd);

	ASSERT3S(size, >, 0);
	ASSERT3S(off, >=, 0);
	ASSERT3S((size_t)abd->abd_size, >=, off+size);
	ASSERT3S((size_t)abd->abd_size, >, 0);

	(void) abd_iterate_func(abd, off, size, abd_zero_off_cb, NULL);
}

/*
 * Iterate over two ABDs and call func incrementally on the two ABDs' data in
 * equal-sized chunks (passed to func as raw buffers). func could be called many
 * times during this iteration.
 */
int
abd_iterate_func2(abd_t *dabd, abd_t *sabd, size_t doff, size_t soff,
    size_t size, abd_iter_func2_t *func, void *private)
{
	int ret = 0;
	struct abd_iter daiter, saiter;

	VERIFY3P(sabd,!=,dabd);

	mutex_enter(&dabd->abd_mutex);
	mutex_enter(&sabd->abd_mutex);
	abd_verify(dabd);
	abd_verify(sabd);

	ASSERT3U(doff + size, <=, (size_t)dabd->abd_size);
	ASSERT3U(soff + size, <=, (size_t)sabd->abd_size);

	abd_iter_init(&daiter, dabd);
	abd_iter_init(&saiter, sabd);
	abd_iter_advance(&daiter, doff);
	abd_iter_advance(&saiter, soff);

	while (size > 0) {
		abd_iter_map(&daiter);
		abd_iter_map(&saiter);

		size_t dlen = MIN(daiter.iter_mapsize, size);
		size_t slen = MIN(saiter.iter_mapsize, size);
		size_t len = MIN(dlen, slen);
		ASSERT(dlen > 0 || slen > 0);

		ret = func(daiter.iter_mapaddr, saiter.iter_mapaddr, len,
		    private);

		abd_iter_unmap(&saiter);
		abd_iter_unmap(&daiter);

		if (ret != 0)
			break;

		size -= len;
		abd_iter_advance(&daiter, len);
		abd_iter_advance(&saiter, len);
	}

	mutex_exit(&sabd->abd_mutex);
	mutex_exit(&dabd->abd_mutex);
	return (ret);
}

/*ARGSUSED*/
static int
abd_copy_off_cb(void *dbuf, void *sbuf, size_t size, void *private)
{
	(void) memcpy(dbuf, sbuf, size);
	return (0);
}

/*
 * Copy from sabd to dabd starting from soff and doff.
 */
void
abd_copy_off(abd_t *dabd, abd_t *sabd, size_t doff, size_t soff, size_t size)
{
	VERIFY_ABD_MAGIC(dabd);
	VERIFY_ABD_MAGIC(sabd);

	ASSERT3S(size, >, 0);
	ASSERT3S(soff, >=, 0);
	ASSERT3S(doff, >=, 0);
	ASSERT3S((size_t)sabd->abd_size, >=, soff+size);
	ASSERT3S((size_t)dabd->abd_size, >=, doff+size);

	(void) abd_iterate_func2(dabd, sabd, doff, soff, size,
	    abd_copy_off_cb, NULL);
}

/*ARGSUSED*/
static int
abd_cmp_cb(void *bufa, void *bufb, size_t size, void *private)
{
	return (memcmp(bufa, bufb, size));
}

/*
 * Compares the first size bytes of two ABDs.
 */
int
abd_cmp(abd_t *dabd, abd_t *sabd, size_t size)
{
	VERIFY_ABD_MAGIC(dabd);
	VERIFY_ABD_MAGIC(sabd);

	ASSERT3P(sabd,!=,NULL);
	ASSERT3P(dabd,!=,NULL);
	ASSERT3P(sabd,!=,dabd);
	ASSERT3S((size_t)sabd->abd_size,==,size);
	ASSERT3S((size_t)dabd->abd_size,==,size);
	return (abd_iterate_func2(dabd, sabd, 0, 0, size, abd_cmp_cb, NULL));
}

#ifdef __APPLE__

/*
 * make a new abd structure with key fields identical to source abd
 * return B_TRUE if we successfully moved the abd
 */

static boolean_t
abd_try_move_scattered_impl(abd_t *abd)
{

	VERIFY0(abd->abd_flags & ABD_FLAG_LINEAR);

	mutex_enter(&abd->abd_mutex);

	abd_verify(abd);

	if (!refcount_is_zero(&abd->abd_children)) {
		mutex_exit(&abd->abd_mutex);
		ABDSTAT_BUMP(abdstat_move_refcount_nonzero);
		return (B_FALSE);
	}

	// from abd_alloc_struct and abd_alloc_free
	const size_t chunkcnt = abd_scatter_chunkcnt(abd);
        const size_t hsize = offsetof(abd_t, abd_u.abd_scatter.abd_chunks[chunkcnt]);
	const size_t asize = abd->abd_size;
	const size_t n = abd_chunkcnt_for_bytes(asize);
	VERIFY3U(n,==,chunkcnt);

	abd_t *partialabd = kmem_zalloc(hsize, KM_PUSHPAGE);
	ASSERT3P(partialabd, !=, NULL);

	partialabd->abd_u.abd_scatter.abd_offset = 0;
	partialabd->abd_u.abd_scatter.abd_chunk_size = zfs_abd_chunk_size;

	// copy abd's chunks into new chunks under partialabd
	for (int i = 0; i < chunkcnt; i++) {
		void *c = abd_alloc_chunk();
		ASSERT3P(c, !=, NULL);
		partialabd->abd_u.abd_scatter.abd_chunks[i] = c;
		(void) memcpy(partialabd->abd_u.abd_scatter.abd_chunks[i],
		    abd->abd_u.abd_scatter.abd_chunks[i], zfs_abd_chunk_size);
	}

	// release abd's old chunks to the kmem_cache
	// and move chunks from partialabd to abd
	for (int j = 0; j < chunkcnt; j++) {
		abd_free_chunk_to_slab(abd->abd_u.abd_scatter.abd_chunks[j]);
		abd->abd_u.abd_scatter.abd_chunks[j] =
		    partialabd->abd_u.abd_scatter.abd_chunks[j];
	}

	// update time
	abd->abd_create_time = gethrtime();

	abd_verify(abd);

	// release partialabd
	kmem_free(partialabd, hsize);

	mutex_exit(&abd->abd_mutex);

	return (B_TRUE);
}

static boolean_t
abd_try_move_linear_impl(abd_t *abd)
{
	ASSERT((abd->abd_flags & ABD_FLAG_LINEAR) == ABD_FLAG_LINEAR);

	mutex_enter(&abd->abd_mutex);

	abd_verify(abd);

	if (!refcount_is_zero(&abd->abd_children)) {
		mutex_exit(&abd->abd_mutex);
		ABDSTAT_BUMP(abdstat_move_refcount_nonzero);
		return (B_FALSE);
	}

	// from abd_alloc_struct(0)
	const size_t hsize = offsetof(abd_t, abd_u.abd_scatter.abd_chunks[0]);
	abd_t *partialabd = kmem_alloc(hsize, KM_PUSHPAGE);
	ASSERT3P(partialabd, !=, NULL);

	const boolean_t is_metadata = (abd->abd_flags & ABD_FLAG_META) == ABD_FLAG_META;
	const size_t bsize = abd->abd_size;

	void *newbuf = NULL;
	if (is_metadata)
		newbuf = zio_buf_alloc(bsize);
	else
		newbuf = zio_data_buf_alloc(bsize);
	ASSERT3P(newbuf, !=, NULL);

	(void) memcpy(newbuf, abd->abd_u.abd_linear.abd_buf, bsize);

	void *oldbuf = abd->abd_u.abd_linear.abd_buf;

	abd->abd_u.abd_linear.abd_buf = newbuf;

	if (is_metadata)
		zio_buf_free(oldbuf, bsize);
	else
		zio_data_buf_free(oldbuf, bsize);

	// update time
	abd->abd_create_time = gethrtime();

	mutex_exit(&abd->abd_mutex);

	kmem_free(partialabd, hsize);

	return(B_TRUE);
}

/* return B_TRUE if we successfully move the abd */

static boolean_t
abd_try_move_impl(abd_t *abd)
{

	if ((abd->abd_flags & ABD_FLAG_NOMOVE) == ABD_FLAG_NOMOVE) {
		ABDSTAT_BUMP(abdstat_move_to_buf_flag_fail);
		ASSERTV(hrtime_t now = gethrtime());
		ASSERTV(hrtime_t fivemin = SEC2NSEC(5*60));
		ASSERT3U((abd->abd_create_time + fivemin),<=,now);
		return (B_FALSE);
	}

	const boolean_t is_metadata = ((abd->abd_flags & ABD_FLAG_META) == ABD_FLAG_META);

	if ((abd->abd_flags & ABD_FLAG_LINEAR) == ABD_FLAG_LINEAR) {
		boolean_t r = abd_try_move_linear_impl(abd);
		if (r == B_TRUE) {
			ABDSTAT_BUMP(abdstat_moved_linear);
			return (B_TRUE);
		} else {
			return (B_FALSE);
		}
	} else {
		boolean_t r = abd_try_move_scattered_impl(abd);
		if (r == B_TRUE) {
			if (is_metadata)
				ABDSTAT_BUMP(abdstat_moved_scattered_metadata);
			else
				ABDSTAT_BUMP(abdstat_moved_scattered_filedata);
			return (B_TRUE);
		} else {
			return (B_FALSE);
		}
	}
}

boolean_t
abd_try_move(abd_t *abd)
{
	abd_verify(abd);
	return(abd_try_move_impl(abd));
}

#ifdef _KERNEL
void
abd_kmem_depot_ws_zero(void)
{
	kmem_depot_ws_zero(abd_chunk_cache);
}
#endif

#endif
