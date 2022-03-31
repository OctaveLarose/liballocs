/* FIXME: currently we use a "trailer" at the end of the chunk, to store
 * metadata.
 * BUT
 * finding the trailer requires a well-known (global) malloc_usable_size call, whereas
 * different allocators bring different metadata. So it should be a per-allocator
 * call.
 * Could use headers instead of trailers, but then this less extensible:
            the user's chunk base is now different from the allocator's, so
            other malloc API calls (mallinfo, etc.) on the same chunk no longer work
            unless we wrap them all.
 * The right way is probably to override malloc_usable_size() and dispatch
 * to the right allocator's... i.e. like dladdr and the libunwind functions,
 * this is a function that we not-so-secretly override with an 'improved',
 * more powerful version.
 */

/* This file uses GNU C extensions */
#define _GNU_SOURCE

#include <sys/types.h>
size_t malloc_usable_size(void *ptr);
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include "liballocs_private.h"
#include "relf.h"

#define ALLOC_EVENT_QUALIFIERS __attribute__((visibility("hidden")))
#include "alloc_events.h"

#include "malloc-meta.h"
#include "generic_malloc_index.h"
#include "pageindex.h"

#ifndef NO_PTHREADS
#define BIG_LOCK \
	lock_ret = pthread_mutex_lock(&mutex); \
	assert(lock_ret == 0);
#define BIG_UNLOCK \
	lock_ret = pthread_mutex_unlock(&mutex); \
	assert(lock_ret == 0);
/* We're recursive only because assertion failures sometimes want to do 
 * asprintf, so try to re-acquire our mutex. */
static pthread_mutex_t mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

#else
#define BIG_LOCK
#define BIG_UNLOCK
#endif

#ifndef NO_TLS
__thread void *__current_allocsite;
__thread void *__current_allocfn;
__thread size_t __current_allocsz;
__thread int __currently_freeing;
__thread int __currently_allocating;
#else
void *__current_allocsite;
void *__current_allocfn;
size_t __current_allocsz;
int __currently_freeing;
int __currently_allocating;
#endif

#ifdef TRACE_GENERIC_MALLOC_INDEX
/* Size the circular buffer of recently freed chunks */
#define RECENTLY_FREED_SIZE 100
#endif

#ifdef TRACE_GENERIC_MALLOC_INDEX
/* Keep a circular buffer of recently freed chunks */
static void *recently_freed[RECENTLY_FREED_SIZE];
static void **next_recently_freed_to_replace = &recently_freed[0];
#endif

unsigned long biggest_allocated_object __attribute__((visibility("protected")));
unsigned long biggest_unpromoted_object __attribute__((visibility("protected")));
int safe_to_call_malloc;

/* The (unsigned) -1 conversion here provokes a compiler warning,
 * which we suppress. There are two ways of doing this.
 * One is to turn the warning off and back on again, clobbering the former setting.
 * Another is, if the GCC version we have allows it (must be > 4.6ish),
 * to use the push/pop mechanism. If we can't pop, we leave it "on" (conservative).
 * To handle the case where we don't have push/pop, 
 * we also suppress pragma warnings, then re-enable them. :-) */
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverflow"

/* First, re-enable the overflow pragma, to be conservative. */
#pragma GCC diagnostic warning "-Woverflow"
/* Now, if we have "pop", we will restore it to its actual former setting. */
#pragma GCC diagnostic pop
#pragma GCC diagnostic warning "-Wpragmas"

__attribute__((visibility("hidden")))
void post_init(void)
{
}
void __liballocs_malloc_post_init(void) __attribute__((alias("post_init")));

__attribute__((visibility("hidden")))
void __generic_malloc_allocator_init(void)
{
	post_init();
}

static inline struct insert *insert_for_chunk(void *userptr);
static void bitmap_delete(struct big_allocation *arena, void *userptr);
static void 
bitmap_insert(struct big_allocation *arena, void *allocptr, size_t requested_size, const void *caller);

void __generic_malloc_bitmap_insert(struct big_allocation *arena, void *allocptr, size_t requested_size,
		const void *caller)
{
	bitmap_insert(arena, allocptr, requested_size, caller);
}
static unsigned long bitmap_insert_count;

#define SHOULD_PROMOTE_TO_BIGALLOC(userchunk, usable_size) \
	((usable_size) > /* HACK: default glibc lower mmap threshold: 128 kB */ 131072)

// this is only used for promotion
static struct big_allocation *fresh_big(void *allocptr, size_t bigalloc_size,
	struct big_allocation *containing_bigalloc)
{
	struct big_allocation *b = __liballocs_new_bigalloc(
		allocptr,
		bigalloc_size,
		NULL /* allocator private */,
		NULL /* allocator_private_free */,
		containing_bigalloc,
		&__generic_malloc_allocator
	);
	if (!b) abort();
	return b;
}
static struct big_allocation *ensure_big(void *addr, size_t size)
{
	void *start;
	struct big_allocation *maybe_already = __lookup_bigalloc_from_root(addr,
		&__generic_malloc_allocator, &start);
	if (maybe_already) return maybe_already;
	return fresh_big(addr, size, __lookup_deepest_bigalloc(addr));
}

// FIXME: I think bigallocs can grow at the beginning as well as at the end.
// That would really screw up our bitmap. Figure out whether that could affect us...
// only some bigallocs, like mapping sequences maybe, can do this.
static void check_arena_bitmap(struct big_allocation *arena)
{
	struct arena_bitmap_info *info = arena->suballocator_private;
	if (unlikely(!info))
	{
		info = arena->suballocator_private = __private_malloc(sizeof (*info));
		arena->suballocator_private_free = __free_arena_bitmap_and_info;
		info->nwords = 0;
		info->bitmap = NULL;
	}
	uintptr_t bitmap_base_addr = (uintptr_t)ROUND_DOWN_PTR(arena->begin, MALLOC_ALIGN*BITMAP_WORD_NBITS);
	unsigned long total_words = ((uintptr_t)(ROUND_UP_PTR(arena->end, MALLOC_ALIGN*BITMAP_WORD_NBITS))
			- bitmap_base_addr)
			/ (MALLOC_ALIGN * BITMAP_WORD_NBITS);
	if (unlikely(info->nwords < total_words))
	{
		info->bitmap = __private_realloc(info->bitmap, total_words * sizeof (bitmap_word_t));
		if (!info->bitmap) abort();
		info->nwords = total_words;
		info->bitmap_base_addr = (void*)bitmap_base_addr;
	}
}
__attribute__((visibility("hidden")))
void __free_arena_bitmap_and_info(void *info /* really struct arena_bitmap_info * */)
{
	struct arena_bitmap_info *the_info = info;
	if (the_info && the_info->bitmap) __private_free(the_info->bitmap);
	if (the_info) __private_free(the_info);
}

struct big_allocation *arena_for_userptr(void *userptr)
{
	struct big_allocation *b = __lookup_bigalloc_from_root_by_suballocator(userptr,
		&__generic_malloc_allocator, NULL);
	// what if we get no b? probably means we're not initialized, e.g. a malloc
	// happening during __runt_files_init.
	assert(b);
	return b;
}

static void bitmap_insert(struct big_allocation *arena, void *allocptr, size_t caller_requested_size, const void *caller)
{
	int lock_ret;
	BIG_LOCK
	assert(arena);
	assert(arena->suballocator == &__generic_malloc_allocator
			|| arena->suballocator == &__alloca_allocator);
	check_arena_bitmap(arena);
	struct arena_bitmap_info *info = (struct arena_bitmap_info *) arena->suballocator_private;
	bitmap_word_t *bitmap = info->bitmap;
	/* The address *must* be in our tracked range. Assert this. */
	assert(info->bitmap_base_addr == ROUND_DOWN_PTR(arena->begin, MALLOC_ALIGN*BITMAP_WORD_NBITS)); // start of coverage (not of bitmap)
	void *bitmap_end_addr = (void*)((uintptr_t) info->bitmap_base_addr +       // limit of coverage
		((struct arena_bitmap_info *) arena->suballocator_private)->nwords * MALLOC_ALIGN * BITMAP_WORD_NBITS);
	assert((uintptr_t) allocptr <= (uintptr_t) bitmap_end_addr);

#ifdef TRACE_GENERIC_MALLOC_INDEX
	/* Check the recently freed list for this pointer. Delete it if we find it. */
	for (int i = 0; i < RECENTLY_FREED_SIZE; ++i)
	{
		if (recently_freed[i] == allocptr)
		{ 
			recently_freed[i] = NULL;
			next_recently_freed_to_replace = &recently_freed[i];
		}
	}
#endif
	size_t alloc_usable_size = malloc_usable_size(allocptr);
	size_t caller_usable_size = caller_usable_size_for_chunk_and_malloc_usable_size(allocptr,
			alloc_usable_size);
	struct insert *p_insert = insert_for_chunk_and_caller_usable_size(allocptr,
		caller_usable_size);
	/* Populate our extra in-chunk fields */
	p_insert->alloc_site_flag = 0U;
	p_insert->alloc_site = (uintptr_t) caller;

#if 0 // def PRECISE_REQUESTED_ALLOCSIZE
	/* FIXME: this isn't really the insert size. It's the insert plus padding.
	 * I'm not sure why/whether we need this. */
	ext_insert->insert_size = alloc_usable_size - caller_requested_size;
#define insert_size ext_insert->insert_size
#else
/* In this case, alignment might mean that we padded the actual request
 * to *more* than requested_size + insert_size.
 * In general caller_requested_size <= alloc_usable_size - insert_size */
#define insert_size (sizeof (struct insert))
#endif
#if 0 // def LIFETIME_POLICIES
	// alloca does not have a lifetime_insert
	if (arena->suballocator == &__generic_malloc_allocator)
	{
		ext_insert->lifetime = MANUAL_DEALLOCATION_FLAG;
	}
#endif

	/* Metadata remains in the chunk */
	biggest_allocated_object = MAX(caller_usable_size, biggest_allocated_object);
	if (__builtin_expect(SHOULD_PROMOTE_TO_BIGALLOC(allocptr, alloc_usable_size), 0))
	{
		void *bigalloc_begin = allocptr;
		assert(caller_requested_size <= alloc_usable_size - insert_size);
		// bigalloc size is the caller-usable size
		struct big_allocation *this_chunk_b = fresh_big(allocptr, caller_usable_size, arena);
		if (!this_chunk_b) abort();
	}
	else
	{
		biggest_unpromoted_object = MAX(caller_usable_size, biggest_unpromoted_object);
	}
#undef insert_size
#ifdef TRACE_GENERIC_MALLOC_INDEX
	fprintf(stderr, "***[%09ld] Inserting user chunk at %p into bitmap at %p\n", 
		bitmap_insert_count, allocptr, bitmap);
#endif
#if !defined(NDEBUG) || defined(TRACE_GENERIC_MALLOC_INDEX)
	++bitmap_insert_count;
#endif
	/* Add it to the bitmap.  */
	bitmap_set_l(bitmap, (allocptr - info->bitmap_base_addr) / MALLOC_ALIGN);
out:
	BIG_UNLOCK
}

void 
post_successful_alloc(void *allocptr, size_t modified_size, size_t modified_alignment, 
		size_t requested_size, size_t requested_alignment, const void *caller)
		__attribute__((visibility("hidden")));
void 
__liballocs_malloc_post_successful_alloc(void *allocptr, size_t modified_size, size_t modified_alignment, 
		size_t requested_size, size_t requested_alignment, const void *caller)
		__attribute__((alias("post_successful_alloc")));
void 
post_successful_alloc(void *allocptr, size_t modified_size, size_t modified_alignment, 
		size_t requested_size, size_t requested_alignment, const void *caller)
{
	bitmap_insert(arena_for_userptr(allocptr), allocptr /* == userptr */, requested_size, __current_allocsite ? __current_allocsite : caller);
	safe_to_call_malloc = 1; // if somebody succeeded, anyone should succeed
}

void pre_alloc(size_t *p_size, size_t *p_alignment, const void *caller) __attribute__((visibility("hidden")));
void __liballocs_malloc_pre_alloc(size_t *p_size, size_t *p_alignment, const void *caller)
	__attribute__((alias("pre_alloc")));
void pre_alloc(size_t *p_size, size_t *p_alignment, const void *caller)
{
	/* We increase the size by the amount of extra data we store, 
	 * and possibly a bit more to allow for alignment.  */
	size_t orig_size = *p_size;
	size_t size_to_allocate = CHUNK_SIZE_WITH_TRAILER(orig_size, struct extended_insert, void*);
	assert(0 == size_to_allocate % ALIGNOF(void *));
	*p_size = size_to_allocate;
}

static void bitmap_delete(struct big_allocation *arena, void *userptr/*, size_t freed_usable_size*/)
{
	/* The freed_usable_size is not strictly necessary. It was added
	 * for handling realloc after-the-fact. In this case, by the time we
	 * get called, the usable size has already changed. However, after-the-fact
	 * was a broken way to handle realloc() when we are using trailers, because in
	 * the case of a *smaller* realloc'd size, where the realloc happens in-place,
	 * realloc() would overwrite our insert with its own (regular heap metadata)
	 * trailer.
	 */

	assert(userptr != NULL);
	void *allocptr = userptr;
	__liballocs_uncache_all(allocptr, malloc_usable_size(allocptr));
	
	int lock_ret;
	BIG_LOCK
	
#ifdef TRACE_GENERIC_MALLOC_INDEX
	/* Check the recently-freed list for this pointer. We will warn about
	 * a double-free if we hit it. */
	for (int i = 0; i < RECENTLY_FREED_SIZE; ++i)
	{
		if (recently_freed[i] == userptr)
		{
			fprintf(stderr, "*** Double free detected for alloc chunk %p\n", 
				userptr);
			return;
		}
	}
#endif
	
	/* Are we a bigalloc? */
	/* If so, we promoted this entry into the bigalloc index. We still
	 * kept its metadata locally, though. */
	struct big_allocation *b = __lookup_bigalloc_under(userptr, &__generic_malloc_allocator,
		arena, NULL);
	if (unlikely(b != NULL))
	{
		void *allocptr = userptr;
		unsigned long size = malloc_usable_size(allocptr);
#ifdef TRACE_GENERIC_MALLOC_INDEX
		fprintf(stderr, "*** Unindexing bigalloc entry for alloc chunk %p (size %lu)\n", 
				allocptr, size);
#endif
		__liballocs_delete_bigalloc_at(userptr, &__generic_malloc_allocator);
#ifdef TRACE_GENERIC_MALLOC_INDEX
		*next_recently_freed_to_replace = userptr;
		++next_recently_freed_to_replace;
		if (next_recently_freed_to_replace == &recently_freed[RECENTLY_FREED_SIZE])
		{
			next_recently_freed_to_replace = &recently_freed[0];
		}
#endif
		BIG_UNLOCK
		return;
	}
	struct arena_bitmap_info *info = (struct arena_bitmap_info *) arena->suballocator_private;
	bitmap_word_t *bitmap = info->bitmap;
	/* The address *must* be in our tracked range. Assert this. */
	assert(info->bitmap_base_addr == ROUND_DOWN_PTR(arena->begin, MALLOC_ALIGN*BITMAP_WORD_NBITS));
	assert((uintptr_t) userptr >= (uintptr_t) info->bitmap_base_addr);
	bitmap_clear_l(bitmap, ((uintptr_t) userptr - (uintptr_t) info->bitmap_base_addr) / 
			(MALLOC_ALIGN * BITMAP_WORD_NBITS));

#ifdef TRACE_GENERIC_MALLOC_INDEX
	fprintf(stderr, "*** Deleting entry for chunk %p, from bitmap at %p\n", 
		userptr, bitmap);
#endif

	/* (old comment; still true?) FIXME: we need a big lock around realloc()
	 * to avoid concurrent in-place realloc()s messing with the other inserts we access. */

out:
#ifdef TRACE_GENERIC_MALLOC_INDEX
	*next_recently_freed_to_replace = userptr;
	++next_recently_freed_to_replace;
	if (next_recently_freed_to_replace == &recently_freed[RECENTLY_FREED_SIZE])
	{
		next_recently_freed_to_replace = &recently_freed[0];
	}
#endif
	BIG_UNLOCK
}
void __generic_malloc_bitmap_delete(struct big_allocation *arena, void *userptr/*, size_t freed_usable_size*/)
{
	bitmap_delete(arena, userptr);
}

int pre_nonnull_free(void *userptr, size_t freed_usable_size) __attribute__((visibility("hidden")));
int __liballocs_malloc_pre_nonnull_free(void *userptr, size_t freed_usable_size)
		__attribute__((alias("pre_nonnull_free")));
int pre_nonnull_free(void *userptr, size_t freed_usable_size)
{
#ifdef LIFETIME_POLICIES
	lifetime_insert_t *lti = lifetime_insert_for_chunk(userptr);
	*lti &= ~MANUAL_DEALLOCATION_FLAG;
	if (*lti) return 1; // Cancel free if we are still alive
	__notify_free(userptr);
#endif
	bitmap_delete(arena_for_userptr(userptr), userptr/*, freed_usable_size*/);
	return 0;
}

void post_nonnull_free(void *userptr) __attribute__((visibility("hidden")));
void __liballocs_malloc_post_nonnull_free(void *userptr) __attribute__((alias("post_nonnull_free")));
void post_nonnull_free(void *userptr) 
{}

void pre_nonnull_nonzero_realloc(void *userptr, size_t size, const void *caller) __attribute__((visibility("hidden")));
void __liballocs_malloc_pre_nonnull_nonzero_realloc(void *userptr, size_t size, const void *caller) 
		__attribute__((alias("pre_nonnull_nonzero_realloc")));
void pre_nonnull_nonzero_realloc(void *userptr, size_t size, const void *caller)
{
	/* When this happens, we *may or may not be freeing an area*
	 * -- i.e. if the realloc fails, we will not actually free anything.
	 * However, whn we were using trailers, and 
	 * in the case of realloc()ing a *slightly smaller* region, 
	 * the allocator might trash our insert (by writing its own data over it). 
	 * So we *must* delete the entry first,
	 * then recreate it later, as it may not survive the realloc() uncorrupted. */
	
	/* Another complication: if we're realloc'ing a bigalloc, we might have to
	 * move its children. BUT should the user ever do this? It's only sensible
	 * to realloc a suballocated area if you know the realloc will happen in-place, 
	 * i.e. if you're making it smaller (only). 
	 * 
	 * BUT some bigallocs are just big; they needn't have children. 
	 * For those, does it matter if we delete and then re-create the bigalloc record?
	 * I don't see why it should.
	 */
	bitmap_delete(arena_for_userptr(userptr), userptr/*, malloc_usable_size(ptr)*/);
}
void post_nonnull_nonzero_realloc(void *userptr, 
	size_t modified_size, 
	size_t old_usable_size,
	const void *caller, void *new_allocptr) __attribute__((visibility("hidden")));
void __liballocs_malloc_post_nonnull_nonzero_realloc(void *userptr, 
	size_t modified_size, 
	size_t old_usable_size,
	const void *caller, void *new_allocptr) __attribute__((alias("post_nonnull_nonzero_realloc")));
void post_nonnull_nonzero_realloc(void *userptr, 
	size_t modified_size, 
	size_t old_usable_size,
	const void *caller, void *new_allocptr)
{
	// FIXME: This requested size could be wrong.
	// The caller should give us the real requested size instead.
	size_t requested_size = __current_allocsz ? __current_allocsz :
		modified_size - sizeof(struct extended_insert);
	/* Are we a bigalloc? */
	struct big_allocation *b = __lookup_bigalloc_from_root(userptr,
			&__generic_malloc_allocator, NULL);
	if (new_allocptr && new_allocptr != userptr)
	{
		/* Create a new bin entry. This will also take care of becoming a bigalloc, etc..
		 * FIXME: check the new type metadata against the old! We can probably do this
		 * in a way that's uniform with memcpy... the new chunk will take its type
		 * from the realloc site, and we then check compatibility on the copy. */
		bitmap_insert(arena_for_userptr(new_allocptr), new_allocptr, requested_size, __current_allocsite ?: caller);
		/* HACK: this is a bit racy. Not sure what to do about it really. We can't
		 * pre-copy (we *could* speculatively pre-snapshot though, into a thread-local
		 * buffer, or a fresh buffer allocated on an "exactly one live per thread" basis). */
		/* FIXME: THIS IS BROKEN when using lifetime extension: userptr is not
		 * pointing to valid memory but is read through... */
#ifndef LIFETIME_POLICIES
		__notify_copy(new_allocptr, userptr, caller_usable_size_for_chunk_and_malloc_usable_size(userptr, old_usable_size));
#endif
	}
	else // !new_allocptr || new_allocptr == userptr
	{
		/* *recreate* the old bin entry! The old usable size
		 * is the *modified* size, i.e. we modified it before
		 * allocating it, so we pass it as the modified_size to
		 * bitmap_insert. */
		// FIXME: is this right? what if new_allocptr is null?
		bitmap_insert(arena_for_userptr(userptr), userptr, requested_size,
			__current_allocsite ? __current_allocsite : caller);
	}
	
	if (new_allocptr == userptr && modified_size < old_usable_size)
	{
		if (b)
		{
			__liballocs_truncate_bigalloc_at_end(b, (char*) userptr + modified_size);
		}
	}

	/* If the old alloc has gone away, do the malloc_hooks call the free hook on it? 
	 * YES: it was done before the realloc, in the pre-hook. */
}

static
struct insert *lookup(struct big_allocation *arena, void *mem, void **out_object_start);

static 
struct insert *object_insert(const void *obj, struct insert *ins)
{
	return ins;
}

/* A client-friendly lookup function. */
struct insert *lookup_object_info(struct big_allocation *arena,
	void *mem, void **out_object_start, size_t *out_object_size, void **ignored)
{
	/* Unlike our malloc hooks, we might get called before initialization,
	   e.g. if someone tries to do a lookup before the first malloc of the
	   program's execution. Rather than putting an initialization check
	   in the fast-path functions, we bail here.  */
	if (!big_allocations[1].begin) return NULL;
	
	void *l01_object_start = NULL;
	struct insert *found_l01 = NULL;
	struct insert *found;
	void *object_start;
	unsigned short depth = 1;
	found = lookup(arena, mem, &l01_object_start);
	size_t size;
	if (found)
	{
		assert(l01_object_start);
		size = usersize(l01_object_start);
		object_start = l01_object_start;
		_Bool is_deepest = INSERT_DESCRIBES_OBJECT(found);

		if (!is_deepest)
		{
			assert(l01_object_start);
			/* deep case */
			void *deep_object_start;
			size_t deep_object_size;
			struct insert *found_deeper = NULL; /*lookup_deep_alloc(mem, 1, found, &deep_object_start, 
					&deep_object_size, &containing_chunk_rec);*/
			if (found_deeper)
			{
				assert(0);
				// override the values we assigned just now
				object_start = deep_object_start;
				found = found_deeper;
				size = deep_object_size;
			}
			else
			{
				// we still have to point the metadata at the *sub*indexed copy
				assert(!INSERT_DESCRIBES_OBJECT(found));
				found = object_insert(mem, found);
			}
		}


		if (out_object_start) *out_object_start = object_start;
		if (out_object_size) *out_object_size = size;
	}
	
	assert(!found || INSERT_DESCRIBES_OBJECT(found));
	return found;
}

static
struct insert *lookup(struct big_allocation *arena, void *mem, void **out_object_start) 
{
	size_t object_minimum_size = 0;
	struct arena_bitmap_info *info = arena->suballocator_private;
	assert(info->bitmap_base_addr == ROUND_DOWN_PTR(arena->begin, MALLOC_ALIGN*BITMAP_WORD_NBITS));
	unsigned start_idx = ((uintptr_t) mem - (uintptr_t) info->bitmap_base_addr) / MALLOC_ALIGN;
	/* OPTIMISATION: since we have a maximum object size,
	 * fake out the bitmap so that we bound the backward search */
	unsigned long nbits_hidden = 0;
#ifdef NDEBUG
	void *fake_bitmap_base_addr = ROUND_DOWN_PTR((uintptr_t) mem -
		(uintptr_t) biggest_unpromoted_object, MALLOC_ALIGN*BITMAP_WORD_NBITS);
	if ((uintptr_t) fake_bitmap_base_addr > (uintptr_t) info->bitmap_base_addr)
	{
		nbits_hidden = BITMAP_WORD_NBITS *
			(((uintptr_t) fake_bitmap_base_addr - (uintptr_t) info->bitmap_base_addr) /
			(MALLOC_ALIGN * BITMAP_WORD_NBITS));
	}
#endif
	assert(nbits_hidden % BITMAP_WORD_NBITS == 0);
	unsigned long found = bitmap_rfind_first_set_leq_l(
		info->bitmap + (nbits_hidden / BITMAP_WORD_NBITS),
		info->bitmap + info->nwords,
		start_idx - nbits_hidden, NULL);
	if (found != (unsigned long) -1)
	{
		found += nbits_hidden;
		void *object_start = info->bitmap_base_addr + (MALLOC_ALIGN * found);
		if (out_object_start) *out_object_start = object_start;
		return insert_for_chunk(object_start);
	}
fail:
	//fprintf(stderr, "Heap index lookup failed for %p with "
	//	"cur_head %p, object_minimum_size %zu, seen_object_starting_earlier %d\n",
	//	mem, cur_head, object_minimum_size, (int) seen_object_starting_earlier);
	return NULL;
}

liballocs_err_t __generic_malloc_get_info(void * obj, struct big_allocation *b, 
	struct uniqtype **out_type, void **out_base, 
	unsigned long *out_size, const void **out_site)
{
	++__liballocs_hit_heap_case;
	/* For heap allocations, we look up the allocation site.
	 * (This also yields an offset within a toplevel object.)
	 * Then we translate the allocation site to a uniqtypes rec location.
	 * (For direct calls in eagerly-loaded code, we can cache this information
	 * within uniqtypes itself. How? Make uniqtypes include a hash table with
	 * initial contents mapping allocsites to uniqtype recs. This hash table
	 * is initialized during load, but can be extended as new allocsites
	 * are discovered, e.g. indirect ones.)
	 */
	struct insert *heap_info = NULL;
	void *base;
	size_t caller_usable_size;
	/* NOTE: bigallocs already have the size adjusted to exclude the insert. */
	if (b && b->allocated_by == &__generic_malloc_allocator)
	{
		/* We already have the metadata. */
		base = b->begin;
		caller_usable_size = (char*) b->end - (char*) b->begin;
		heap_info = insert_for_chunk_and_caller_usable_size(base, caller_usable_size
			+ sizeof (struct extended_insert));
	}
	else
	{
		size_t alloc_usable_chunksize = 0;
		heap_info = lookup_object_info(arena_for_userptr(obj),
			obj, &base, &alloc_usable_chunksize, NULL);
		if (!heap_info)
		{
			/* For an unindexed non-promoted chunk, we don't know the base, so
			 * we don't know the logical size. We don't know anything. Note that
			 * for promoted chunks, we might know the size and base because we
			 * can promote to bigalloc knowing just the original base pointer, from
			 * which malloc_usable_size() can do the rest. */
			++__liballocs_aborted_unindexed_heap;
			return &__liballocs_err_unindexed_heap_object;
		}
		assert(base);
		caller_usable_size = caller_usable_size_for_chunk_and_malloc_usable_size(base,
			alloc_usable_chunksize);
	}
	assert(heap_info);
	if (out_base) *out_base = base;
	if (out_size) *out_size = caller_usable_size;
	if (out_type || out_site) return extract_and_output_alloc_site_and_type(
		heap_info, out_type, (void**) out_site);
	// no error
	return NULL;
}

liballocs_err_t __generic_malloc_set_type(struct big_allocation *maybe_the_allocation, void *obj, struct uniqtype *new_type)
{
	struct insert *ins = lookup_object_info(arena_for_userptr(obj), obj, NULL, NULL, NULL);
	if (!ins) return &__liballocs_err_unindexed_heap_object;
	ins->alloc_site = (uintptr_t) new_type;
	ins->alloc_site_flag = 1; // meaning it's a type, not a site
	return NULL;
}

struct allocator __generic_malloc_allocator = {
	.name = "generic malloc",
	.get_info = __generic_malloc_get_info,
	.is_cacheable = 1,
	.ensure_big = ensure_big,
	.set_type = __generic_malloc_set_type,
	.free = (void (*)(struct allocated_chunk *)) free,
};
