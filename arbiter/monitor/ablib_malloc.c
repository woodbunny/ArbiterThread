/*
  This is a version (aka dlmalloc) of malloc/free/realloc written by
  Doug Lea and released to the public domain.  Use, modify, and
  redistribute this code without permission or acknowledgement in any
  way you wish.  Send questions, comments, complaints, performance
  data, etc to dl@cs.oswego.edu

  VERSION 2.7.2 Sat Aug 17 09:07:30 2002  Doug Lea  (dl at gee)

  Note: There may be an updated version of this malloc obtainable at
           ftp://gee.cs.oswego.edu/pub/misc/malloc.c
  Check before installing!

  Hacked up for uClibc by Erik Andersen <andersen@codepoet.org>
*/

#include "ablib_malloc.h"

pthread_mutex_t __malloc_lock = PTHREAD_MUTEX_INITIALIZER; // ? PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP

/*
   There is exactly one instance of this struct in this malloc.
   If you are adapting this malloc in a way that does NOT use a static
   malloc_state, you MUST explicitly zero-fill it before using. This
   malloc relies on the property that malloc_state is initialized to
   all zeroes (as is true of C statics).
*/
struct abheap_state __abheap_state;	/* never directly referenced */

/* forward declaration */
static int __malloc_largebin_index(unsigned int sz);

#ifdef __UCLIBC_MALLOC_DEBUGGING__

/*
  Debugging support FIXME 1) add av to the arg; 2) take unit into consideration

  Because freed chunks may be overwritten with bookkeeping fields, this
  malloc will often die when freed memory is overwritten by user
  programs.  This can be very effective (albeit in an annoying way)
  in helping track down dangling pointers.

  If you compile with __UCLIBC_MALLOC_DEBUGGING__, a number of assertion checks are
  enabled that will catch more memory errors. You probably won't be
  able to make much sense of the actual assertion errors, but they
  should help you locate incorrectly overwritten memory.  The
  checking is fairly extensive, and will slow down execution
  noticeably. Calling malloc_stats or mallinfo with __UCLIBC_MALLOC_DEBUGGING__ set will
  attempt to check every non-mmapped allocated and free chunk in the
  course of computing the summmaries. (By nature, mmapped regions
  cannot be checked very much automatically.)

  Setting __UCLIBC_MALLOC_DEBUGGING__ may also be helpful if you are trying to modify
  this code. The assertions in the check routines spell out in more
  detail the assumptions and invariants underlying the algorithms.

  Setting __UCLIBC_MALLOC_DEBUGGING__ does NOT provide an automated mechanism for checking
  that all accesses to malloced memory stay within their
  bounds. However, there are several add-ons and adaptations of this
  or other mallocs available that do this.
*/

/* Properties of all chunks */
void __do_check_chunk(mchunkptr p)
{
    mstate av = get_malloc_state();
#ifdef __DOASSERTS__
    /* min and max possible addresses assuming contiguous allocation */
    char* max_address = (char*)(av->top) + chunksize(av->top);
    char* min_address = max_address - av->sbrked_mem;
    unsigned long  sz = chunksize(p);
#endif

    if (!chunk_is_mmapped(p)) {

	/* Has legal address ... */
	if (p != av->top) {
	    if (contiguous(av)) {
		assert(((char*)p) >= min_address);
		assert(((char*)p + sz) <= ((char*)(av->top)));
	    }
	}
	else {
	    /* top size is always at least MINSIZE */
	    assert((unsigned long)(sz) >= MINSIZE);
	    /* top predecessor always marked inuse */
	    assert(prev_inuse(p));
	}

    }
    else {
	/* address is outside main heap  */
	if (contiguous(av) && av->top != initial_top(av)) {
	    assert(((char*)p) < min_address || ((char*)p) > max_address);
	}
	/* chunk is page-aligned */
	assert(((p->prev_size + sz) & (av->pagesize-1)) == 0);
	/* mem is aligned */
	assert(aligned_OK(chunk2mem(p)));
    }
}

/* Properties of free chunks */
void __do_check_free_chunk(mchunkptr p)
{
    size_t sz = p->size & ~PREV_INUSE;
#ifdef __DOASSERTS__
    mstate av = get_malloc_state();
    mchunkptr next = chunk_at_offset(p, sz);
#endif

    __do_check_chunk(p);

    /* Chunk must claim to be free ... */
    assert(!inuse(p));
    assert (!chunk_is_mmapped(p));

    /* Unless a special marker, must have OK fields */
    if ((unsigned long)(sz) >= MINSIZE)
    {
	assert((sz & MALLOC_ALIGN_MASK) == 0);
	assert(aligned_OK(chunk2mem(p)));
	/* ... matching footer field */
	assert(next->prev_size == sz);
	/* ... and is fully consolidated */
	assert(prev_inuse(p));
	assert (next == av->top || inuse(next));

	/* ... and has minimally sane links */
	assert(p->fd->bk == p);
	assert(p->bk->fd == p);
    }
    else /* markers are always of size (sizeof(size_t)) */
	assert(sz == (sizeof(size_t)));
}

/* Properties of inuse chunks */
void __do_check_inuse_chunk(mchunkptr p)
{
    mstate av = get_malloc_state();
    mchunkptr next;
    __do_check_chunk(p);

    if (chunk_is_mmapped(p))
	return; /* mmapped chunks have no next/prev */

    /* Check whether it claims to be in use ... */
    assert(inuse(p));

    next = next_chunk(p);

    /* ... and is surrounded by OK chunks.
       Since more things can be checked with free chunks than inuse ones,
       if an inuse chunk borders them and debug is on, it's worth doing them.
       */
    if (!prev_inuse(p))  {
	/* Note that we cannot even look at prev unless it is not inuse */
	mchunkptr prv = prev_chunk(p);
	assert(next_chunk(prv) == p);
	__do_check_free_chunk(prv);
    }

    if (next == av->top) {
	assert(prev_inuse(next));
	assert(chunksize(next) >= MINSIZE);
    }
    else if (!inuse(next))
	__do_check_free_chunk(next);
}

/* Properties of chunks recycled from fastbins */
void __do_check_remalloced_chunk(mchunkptr p, size_t s)
{
#ifdef __DOASSERTS__
    size_t sz = p->size & ~PREV_INUSE;
#endif

    __do_check_inuse_chunk(p);

    /* Legal size ... */
    assert((sz & MALLOC_ALIGN_MASK) == 0);
    assert((unsigned long)(sz) >= MINSIZE);
    /* ... and alignment */
    assert(aligned_OK(chunk2mem(p)));
    /* chunk is less than MINSIZE more than request */
    assert((long)(sz) - (long)(s) >= 0);
    assert((long)(sz) - (long)(s + MINSIZE) < 0);
}

/* Properties of nonrecycled chunks at the point they are malloced */
void __do_check_malloced_chunk(mchunkptr p, size_t s)
{
    /* same as recycled case ... */
    __do_check_remalloced_chunk(p, s);

    /*
       ... plus,  must obey implementation invariant that prev_inuse is
       always true of any allocated chunk; i.e., that each allocated
       chunk borders either a previously allocated and still in-use
       chunk, or the base of its memory arena. This is ensured
       by making all allocations from the the `lowest' part of any found
       chunk.  This does not necessarily hold however for chunks
       recycled via fastbins.
       */

    assert(prev_inuse(p));
}


/*
  Properties of malloc_state.

  This may be useful for debugging malloc, as well as detecting user
  programmer errors that somehow write into malloc_state.

  If you are extending or experimenting with this malloc, you can
  probably figure out how to hack this routine to print out or
  display chunk addresses, sizes, bins, and other instrumentation.
*/
void __do_check_malloc_state(void)
{
    mstate av = get_malloc_state();
    int i;
    mchunkptr p;
    mchunkptr q;
    mbinptr b;
    unsigned int binbit;
    int empty;
    unsigned int idx;
    size_t size;
    unsigned long  total = 0;
    int max_fast_bin;

    /* internal size_t must be no wider than pointer type */
    assert(sizeof(size_t) <= sizeof(char*));

    /* alignment is a power of 2 */
    assert((MALLOC_ALIGNMENT & (MALLOC_ALIGNMENT-1)) == 0);

    /* cannot run remaining checks until fully initialized */
    if (av->top == 0 || av->top == initial_top(av))
	return;

    /* pagesize is a power of 2 */
    assert((av->pagesize & (av->pagesize-1)) == 0);

    /* properties of fastbins */

    /* max_fast is in allowed range */
    assert(get_max_fast(av) <= request2size(MAX_FAST_SIZE));

    max_fast_bin = fastbin_index(av->max_fast);

    for (i = 0; i < NFASTBINS; ++i) {
	p = av->fastbins[i];

	/* all bins past max_fast are empty */
	if (i > max_fast_bin)
	    assert(p == 0);

	while (p != 0) {
	    /* each chunk claims to be inuse */
	    __do_check_inuse_chunk(p);
	    total += chunksize(p);
	    /* chunk belongs in this bin */
	    assert(fastbin_index(chunksize(p)) == i);
	    p = p->fd;
	}
    }

    if (total != 0)
	assert(have_fastchunks(av));
    else if (!have_fastchunks(av))
	assert(total == 0);

    /* check normal bins */
    for (i = 1; i < NBINS; ++i) {
	b = bin_at(av,i);

	/* binmap is accurate (except for bin 1 == unsorted_chunks) */
	if (i >= 2) {
	    binbit = get_binmap(av,i);
	    empty = last(b) == b;
	    if (!binbit)
		assert(empty);
	    else if (!empty)
		assert(binbit);
	}

	for (p = last(b); p != b; p = p->bk) {
	    /* each chunk claims to be free */
	    __do_check_free_chunk(p);
	    size = chunksize(p);
	    total += size;
	    if (i >= 2) {
		/* chunk belongs in bin */
		idx = bin_index(size);
		assert(idx == i);
		/* lists are sorted */
		if ((unsigned long) size >= (unsigned long)(FIRST_SORTED_BIN_SIZE)) {
		    assert(p->bk == b ||
			    (unsigned long)chunksize(p->bk) >=
			    (unsigned long)chunksize(p));
		}
	    }
	    /* chunk is followed by a legal chain of inuse chunks */
	    for (q = next_chunk(p);
		    (q != av->top && inuse(q) &&
		     (unsigned long)(chunksize(q)) >= MINSIZE);
		    q = next_chunk(q))
		__do_check_inuse_chunk(q);
	}
    }

    /* top chunk is OK */
    __do_check_chunk(av->top);

    /* sanity checks for statistics */

    assert(total <= (unsigned long)(av->max_total_mem));
    assert(av->n_mmaps >= 0);
    assert(av->n_mmaps <= av->max_n_mmaps);

    assert((unsigned long)(av->sbrked_mem) <=
	    (unsigned long)(av->max_sbrked_mem));

    assert((unsigned long)(av->mmapped_mem) <=
	    (unsigned long)(av->max_mmapped_mem));

    assert((unsigned long)(av->max_total_mem) >=
	    (unsigned long)(av->mmapped_mem) + (unsigned long)(av->sbrked_mem));
}
#endif


/* ----------- Routines dealing with system allocation -------------- */

/*
  sysmalloc handles malloc cases requiring more memory from the system.
  On entry, it is assumed that av->top does not have enough
  space to service request for nb bytes, thus requiring that av->top
  be extended or replaced.
*/
static void* __malloc_alloc(pid_t pid, size_t nb, mstate av, label_t L)
{
	mchunkptr	old_ab_top;	/* incoming value of av->top */
	size_t		old_max_top_size;	/* its size */
	char*		old_ab_end;	/* its end address */

	long		size;		/* arg to first MORECORE or mmap call */
	char*		fst_brk;	/* return value from MORECORE */
	char*		ab_fst_brk;	/* return value from AB_MORECORE */

	long		correction;	/* arg to 2nd MORECORE call */
	char*		snd_brk;	/* 2nd return val */
	char*		ab_snd_brk;	/* 2nd return val from AB_MORECORE*/

	size_t		front_misalign;	/* unusable bytes at front of new space */
	size_t		end_misalign;	/* partial page left at end of new space */
	char*		aligned_brk;	/* aligned offset into brk */

	mchunkptr	unit_top;
	size_t		unit_top_size;
	ustate		unit;

	mchunkptr	p;		/* the allocated/returned chunk */
	mchunkptr	remainder;	/* remainder from allocation */
	unsigned long	remainder_size;	/* its size */

	unsigned long	sum;		/* for updating stats */

	size_t		pagemask  = av->pagesize - 1;
	    
	mchunkptr	fwd;		/* misc temp for linking */
	mchunkptr	bck;		/* misc temp for linking */

	//AB_INFO("__malloc_alloc called: arguments = (%d, %d, av, %lx, %lx)\n", 
	//	pid, nb, *(unsigned long *)L, (void *)L);

	/*
	   If there is space available in fastbins, consolidate and retry
	   malloc from scratch rather than getting memory from system.  This
	   can occur only if nb is in smallbin range so we didn't consolidate
	   upon entry to malloc. It is much easier to handle this case here
	   than in malloc proper.
	   */

	if (have_fastchunks(av)) {
		assert(in_smallbin_range(nb));
		__malloc_consolidate(av);
		return (void *)ablib_malloc(pid, nb - MALLOC_ALIGN_MASK, L);
	}

	/*
	   If have mmap, and the request size meets the mmap threshold, and
	   the system supports mmap, and there are few enough currently
	   allocated mmapped regions, try to directly map this request
	   rather than expanding top.
	   */

	if ((unsigned long)(nb) >= (unsigned long)(av->mmap_threshold) &&
		(av->n_mmaps < av->n_mmaps_max)) {

		char* mm;	/* return value from mmap call*/
		char* ab_mm;	/* return value from AB_MMAP call*/

		/*
		   Round up size to nearest page.  For mmapped chunks, the overhead
		   is one (sizeof(size_t)) unit larger than for normal chunks, because there
		   is no following chunk whose prev_size field could be used.
		   */
		size = (nb + (sizeof(size_t)) + MALLOC_ALIGN_MASK + pagemask) & ~pagemask;

		/* Don't try if size wraps around 0 */
		if ((unsigned long)(size) > (unsigned long)(nb)) {
			//touch the memory	//not needed
			//mm = (char *)mmap(NULL, size, PROT_READ|PROT_WRITE, 
			//	MAP_ANONYMOUS|MAP_SHARED, -1, 0);
			//assert(mm != (char *)(-1));
			//touch_mem(mm, size);

			mm = (char *)get_unmapped_area(&(get_abstate()->mmapped_ustate_list), size);

			if (mm != (char *)MORECORE_FAILURE) {
				/*
				   The offset to the start of the mmapped region is stored
				   in the prev_size field of the chunk. This allows us to adjust
				   returned start address to meet alignment requirements here
				   and in memalign(), and still be able to compute proper
				   address argument for later munmap in free() and realloc().
				   */

				front_misalign = (size_t)chunk2mem(mm) & MALLOC_ALIGN_MASK;
				if (front_misalign > 0) {
					correction = MALLOC_ALIGNMENT - front_misalign;
					p = (mchunkptr)(mm + correction);
					p->prev_size = correction;
					set_head(p, (size - correction) |IS_MMAPPED);
				}
				else {
					p = (mchunkptr)mm;
					p->prev_size = 0;
					set_head(p, size|IS_MMAPPED);
				}
				
#ifdef _SYSCALL_COUNT_TIME
				syscall_count[MMAP] += 1;
				uint64_t start = rdtsc();
#endif
				ab_mm = (char*)(AB_MMAP(pid, (void *)mm, size, PROT_READ|PROT_WRITE));
#ifdef _SYSCALL_COUNT_TIME
				uint64_t end = rdtsc();
				syscall_time[MMAP] += end - start;
				printf("SYSCALL MMAP: count %d, time %0.2fus\n",
					syscall_count[MMAP],
					syscall_time[MMAP]/syscall_count[MMAP]/_CPU_FRQ);
#endif
				assert(ab_mm == (char*)mm);

				if (ab_mm != (char*)(MORECORE_FAILURE)) {
					//add unit state
					unit = unit_init_state((mchunkptr)mm, (unsigned long)mm, size, av);
					list_insert_head(&(get_abstate()->ustate_list), unit);
					insert_mmapped_unit(&(get_abstate()->mmapped_ustate_list), unit);

					/* update statistics */

					if (++av->n_mmaps > av->max_n_mmaps)
						av->max_n_mmaps = av->n_mmaps;

					sum = av->mmapped_mem += size;
					if (sum > (unsigned long)(av->max_mmapped_mem))
						av->max_mmapped_mem = sum;
					sum += av->sbrked_mem;
					if (sum > (unsigned long)(av->max_total_mem))
						av->max_total_mem = sum;

					check_chunk(p);
		
					//update protection for other threads
					prot_update(pid, mm, size, L);

					return chunk2mem(p);
				}
				// not needed
				//else {
				//	munmap(mm, size);
				//}
			}
		}
	}

	/* Record incoming configuration of top */

	old_ab_top = get_abstate()->ab_top;
	old_ab_end = (char *)old_ab_top;
	//old_max_top_size = chunksize(get_ustate((&(av->ustate_list))->tail)->unit_top); 
	//old_end  = (char*)(chunk_at_offset(old_top, old_size));

	fst_brk = snd_brk = (char*)(MORECORE_FAILURE);

	///* If not the first time through, we require old_size to
	// * be at least MINSIZE and to have prev_inuse set.  */

	//assert((old_top == initial_top(av) && old_size == 0) ||
	//    ((unsigned long) (old_size) >= MINSIZE &&
	//     prev_inuse(old_top)));

	///* Precondition: not enough current space to satisfy nb request */
	//assert((unsigned long)(old_max_top_size) < (unsigned long)(nb + MINSIZE));

	/* Precondition: all fastbins are consolidated */
	assert(!have_fastchunks(av));

	/* Request UNIT_SIZE memory each time */
	size = UNIT_SIZE;

	/*
	   Don't try to call MORECORE if argument is so big as to appear
	   negative. Note that since mmap takes size_t arg, it may succeed
	   below even if we cannot call MORECORE.
	   */
	if (size > 0) {
#ifdef _SYSCALL_COUNT_TIME
		syscall_count[SBRK] += 1;
		uint64_t start = rdtsc();
#endif
		fst_brk = (char*)(AB_MORECORE(pid, size));
#ifdef _SYSCALL_COUNT_TIME
		uint64_t end = rdtsc();
		syscall_time[SBRK] += end - start;
		printf("SYSCALL SBRK: count %d, time %0.2fus\n",
			syscall_count[SBRK],
			syscall_time[SBRK]/syscall_count[SBRK]/_CPU_FRQ);
#endif
		assert(fst_brk != (char*)(MORECORE_FAILURE));
	}
	/*
	   If have mmap, try using it as a backup when MORECORE fails or
	   cannot be used. This is worth doing on systems that have "holes" in
	   address space, so sbrk cannot extend to give contiguous space, but
	   space is available elsewhere.  Note that we ignore mmap max count
	   and threshold limits, since the space will not be used as a
	   segregated mmap region.
	   */
	//this should less likely happen!
	if (fst_brk == (char*)(MORECORE_FAILURE)) {
		fst_brk = (char*)(AB_MMAP(pid, 0, size, PROT_READ|PROT_WRITE));
		
		if (fst_brk != (char*)(MORECORE_FAILURE)) {
			fst_brk = (char *)mmap(NULL, size, PROT_READ|PROT_WRITE, 
				MAP_ANONYMOUS|MAP_SHARED, -1, 0);
			assert(fst_brk != (char *)(-1));	//FIXME: BUG if mmap fails

			//touch the memory
			touch_mem(fst_brk, size);

			/* We do not need, and cannot use, another sbrk call to find end */
			snd_brk = fst_brk + size;

			/* Record that we no longer have a contiguous sbrk region.
			   After the first time mmap is used as backup, we do not
			   ever rely on contiguous space since this could incorrectly
			   bridge regions.
			   */
			set_noncontiguous(av);
		}
	}
	else { //fst_brk != (char*)(MORECORE_FAILURE)

		av->sbrked_mem += size;

		/*
		 * We need to ensure that all returned chunks from malloc will meet
		 MALLOC_ALIGNMENT

		 * Almost all systems internally allocate whole pages at a time, in
		 which case we might as well use the whole last page of request.
		 So we allocate enough more memory to hit a page boundary now,
		 which in turn causes future contiguous calls to page-align.
		 */
	
		//	/* MORECORE/mmap must correctly align */
		assert(aligned_OK(chunk2mem(fst_brk)));

		unit_top = (mchunkptr)fst_brk;

		//add unit state
		unit = unit_init_state(unit_top, (unsigned long)fst_brk, size, av);
		assert(size == UNIT_SIZE);
		list_insert_head(&(av->ustate_list), unit);
		list_insert_head(&(get_abstate()->ustate_list), unit);
		/*
		   If not the first time through, we either have a
		   gap due to foreign sbrk or a non-contiguous region.  Insert a
		   double fencepost at old_top to prevent consolidation with space
		   we don't own. These fenceposts are artificial chunks that are
		   marked as inuse and are in any case too small to use.  We need
		   two to make sizes and alignments work out.
		   */

		/* Shrink old_top to insert fenceposts, keeping size a
		   multiple of MALLOC_ALIGNMENT. We know there is at least
		   enough space in old_top to do this.
		   */
		unit_top_size = (size - 3*(sizeof(size_t))) & ~MALLOC_ALIGN_MASK;
		set_head(unit_top, unit_top_size | PREV_INUSE);

		/*
		   Note that the following assignments completely overwrite
		   old_top when old_size was previously MINSIZE.  This is
		   intentional. We need the fencepost, even if old_top otherwise gets
		   lost.
		   */
		chunk_at_offset(unit_top, unit_top_size)->size =
			(sizeof(size_t))|PREV_INUSE;

		chunk_at_offset(unit_top, unit_top_size + (sizeof(size_t)))->size =
			(sizeof(size_t))|PREV_INUSE;


		/* Update statistics */
		sum = av->sbrked_mem;
		if (sum > (unsigned long)(av->max_sbrked_mem))
		    av->max_sbrked_mem = sum;

		sum += av->mmapped_mem;
		if (sum > (unsigned long)(av->max_total_mem))
		    av->max_total_mem = sum;

		check_malloc_state();

		/* finally, do the allocation */

		p = unit_top;
		size = chunksize(p);

		/* check that one of the above allocation paths succeeded */
		if ((unsigned long)(size) >= (unsigned long)(nb + MINSIZE)) {
			remainder_size = size - nb;
			remainder = chunk_at_offset(p, nb);
			unit_top = remainder;

			//update unit_top infomation in ustate
			unit->unit_top = unit_top;
		
			/* place remainder back to the topbin */		
			//bck = av->ustate_list;
			//fwd = bck->fd;

			//if (fwd != bck) {
			///* if smaller than smallest, place first */
			//	if ((unsigned long)(size) < 
			//		(unsigned long)(bck->bk->size)) {
			//		fwd = bck;
			//		bck = bck->bk;
			//	}
			//	else if ((unsigned long)(size) >=
			//		(unsigned long)(MINSIZE)) {

			//	    /* maintain topbin in sorted order */
			//		size |= PREV_INUSE; /* Or with inuse bit to speed comparisons */
			//		while ((unsigned long)(size) < (unsigned long)(fwd->size))
			//			fwd = fwd->fd;
			//	        bck = fwd->bk;
			//	}
			//}
			//remainder->bk = bck;
			//remainder->fd = fwd;
			//fwd->bk = remainder;
			//bck->fd = remainder;

			set_head(p, nb | PREV_INUSE);
			set_head(remainder, remainder_size | PREV_INUSE);
			set_foot(remainder, remainder_size);
			check_malloced_chunk(p, nb);
		
			//update protection for other threads
			prot_update(pid, p, size, L);

			return chunk2mem(p);
		}
	}

	/* catch all failure paths */
	errno = ENOMEM;
	return 0;
}


/*
  Compute index for size. We expect this to be inlined when
  compiled with optimization, else not, which works out well.
*/
static int __malloc_largebin_index(unsigned int sz)
{
    unsigned int  x = sz >> SMALLBIN_WIDTH;
    unsigned int m;            /* bit position of highest set bit of m */

    if (x >= 0x10000) return NBINS-1;

    /* On intel, use BSRL instruction to find highest bit */
#if defined(__GNUC__) && defined(i386)

    __asm__("bsrl %1,%0\n\t"
	    : "=r" (m)
	    : "g"  (x));

#else
    {
	/*
	   Based on branch-free nlz algorithm in chapter 5 of Henry
	   S. Warren Jr's book "Hacker's Delight".
	   */

	unsigned int n = ((x - 0x100) >> 16) & 8;
	x <<= n;
	m = ((x - 0x1000) >> 16) & 4;
	n += m;
	x <<= m;
	m = ((x - 0x4000) >> 16) & 2;
	n += m;
	x = (x << m) >> 14;
	m = 13 - n + (x & ~(x>>1));
    }
#endif

    /* Use next 2 bits to create finer-granularity bins */
    return NSMALLBINS + (m << 2) + ((sz >> (m + 6)) & 3);
}



/* ----------------------------------------------------------------------
 *
 * PUBLIC STUFF
 *
 * ----------------------------------------------------------------------*/


/* ------------------------------ ablib_malloc ------------------------------ */
void *ablib_malloc(pid_t pid, size_t bytes, label_t L)
{
	mstate av;

	size_t nb;			/* normalized request size */
	unsigned int	idx;		/* associated bin index */
	mbinptr		bin;		/* associated bin */
	mfastbinptr*	fb;		/* associated fastbin */

	mchunkptr	victim;		/* inspected/selected chunk */
	size_t size;			/* its size */
	int		victim_index;	/* its bin index */
	struct list_node	*victim_unit;

	mchunkptr	remainder;	/* remainder from a split */
	unsigned long	remainder_size;	/* its size */

	unsigned int	block;		/* bit map traverser */
	unsigned int	bit;		/* bit map traverser */
	unsigned int	map;		/* current word of binmap */

	mchunkptr	fwd;		/* misc temp for linking */
	mchunkptr	bck;		/* misc temp for linking */
	void *		sysmem;
	void *		retval;

	AB_INFO("ablib_malloc called: arguments = (%d, %d, %lx)\n", pid, bytes, *(unsigned long *)L);
	//get av
	av = lookup_mstate_by_label(L);

#if !defined(__MALLOC_GLIBC_COMPAT__)
	if (!bytes) {
	    __set_errno(ENOMEM);
	    return NULL;
	}
#endif

    __MALLOC_LOCK;

    /* If call ablib_malloc with a new label, allocate mstate */
    if (av == NULL) {
    	av = (mstate)malloc(sizeof(struct malloc_state));
    	memset(av, 0, sizeof(struct malloc_state));
    }
    
    /*
       Convert request size to internal form by adding (sizeof(size_t)) bytes
       overhead plus possibly more to obtain necessary alignment and/or
       to obtain a size of at least MINSIZE, the smallest allocatable
       size. Also, checked_request2size traps (returning 0) request sizes
       that are so large that they wrap around zero when padded and
       aligned.
       */

    checked_request2size(bytes, nb);

    /*
       Bypass search if no frees yet
       */
    if (!have_anychunks(av)) {
	if (av->max_fast == 0) {/* initialization check */
	    __malloc_consolidate(av);  //initialization purpose
	    memcpy(av->label, L, sizeof(label_t));
	}
	goto use_unit_top;
    }

    /*
       If the size qualifies as a fastbin, first check corresponding bin.
       */

    if ((unsigned long)(nb) <= (unsigned long)(av->max_fast)) {
	fb = &(av->fastbins[(fastbin_index(nb))]);
	if ( (victim = *fb) != 0) {
	    *fb = victim->fd;
	    check_remalloced_chunk(victim, nb);
	    retval = chunk2mem(victim);
	    goto DONE;
	}
    }

    /*
       If a small request, check regular bin.  Since these "smallbins"
       hold one size each, no searching within bins is necessary.
       (For a large request, we need to wait until unsorted chunks are
       processed to find best fit. But for small ones, fits are exact
       anyway, so we can check now, which is faster.)
       */

    if (in_smallbin_range(nb)) {
	idx = smallbin_index(nb);
	bin = bin_at(av,idx);

	if ( (victim = last(bin)) != bin) {
	    bck = victim->bk;
	    set_inuse_bit_at_offset(victim, nb);
	    bin->bk = bck;
	    bck->fd = bin;

	    check_malloced_chunk(victim, nb);
	    retval = chunk2mem(victim);
	    goto DONE;
	}
    }

    /* If this is a large request, consolidate fastbins before continuing.
       While it might look excessive to kill all fastbins before
       even seeing if there is space available, this avoids
       fragmentation problems normally associated with fastbins.
       Also, in practice, programs tend to have runs of either small or
       large requests, but less often mixtures, so consolidation is not
       invoked all that often in most programs. And the programs that
       it is called frequently in otherwise tend to fragment.
       */

    else {
	idx = __malloc_largebin_index(nb);
	if (have_fastchunks(av))
	    __malloc_consolidate(av);
    }

    /*
       Process recently freed or remaindered chunks, taking one only if
       it is exact fit, or, if this a small request, the chunk is remainder from
       the most recent non-exact fit.  Place other traversed chunks in
       bins.  Note that this step is the only place in any routine where
       chunks are placed in bins.
       */

    while ( (victim = unsorted_chunks(av)->bk) != unsorted_chunks(av)) {
	bck = victim->bk;
	size = chunksize(victim);

	/* If a small request, try to use last remainder if it is the
	   only chunk in unsorted bin.  This helps promote locality for
	   runs of consecutive small requests. This is the only
	   exception to best-fit, and applies only when there is
	   no exact fit for a small chunk.
	   */

	if (in_smallbin_range(nb) &&
		bck == unsorted_chunks(av) &&
		victim == av->last_remainder &&
		(unsigned long)(size) > (unsigned long)(nb + MINSIZE)) {

	    /* split and reattach remainder */
	    remainder_size = size - nb;
	    remainder = chunk_at_offset(victim, nb);
	    unsorted_chunks(av)->bk = unsorted_chunks(av)->fd = remainder;
	    av->last_remainder = remainder;
	    remainder->bk = remainder->fd = unsorted_chunks(av);

	    set_head(victim, nb | PREV_INUSE);
	    set_head(remainder, remainder_size | PREV_INUSE);
	    set_foot(remainder, remainder_size);

	    check_malloced_chunk(victim, nb);
	    retval = chunk2mem(victim);
	    goto DONE;
	}

	/* remove from unsorted list */
	unsorted_chunks(av)->bk = bck;
	bck->fd = unsorted_chunks(av);

	/* Take now instead of binning if exact fit */

	if (size == nb) {
	    set_inuse_bit_at_offset(victim, size);
	    check_malloced_chunk(victim, nb);
	    retval = chunk2mem(victim);
	    goto DONE;
	}

	/* place chunk in bin */

	if (in_smallbin_range(size)) {
	    victim_index = smallbin_index(size);
	    bck = bin_at(av, victim_index);
	    fwd = bck->fd;
	}
	else {
	    victim_index = __malloc_largebin_index(size);
	    bck = bin_at(av, victim_index);
	    fwd = bck->fd;

	    if (fwd != bck) {
		/* if smaller than smallest, place first */
		if ((unsigned long)(size) < (unsigned long)(bck->bk->size)) {
		    fwd = bck;
		    bck = bck->bk;
		}
		else if ((unsigned long)(size) >=
			(unsigned long)(FIRST_SORTED_BIN_SIZE)) {

		    /* maintain large bins in sorted order */
		    size |= PREV_INUSE; /* Or with inuse bit to speed comparisons */
		    while ((unsigned long)(size) < (unsigned long)(fwd->size))
			fwd = fwd->fd;
		    bck = fwd->bk;
		}
	    }
	}

	mark_bin(av, victim_index);
	victim->bk = bck;
	victim->fd = fwd;
	fwd->bk = victim;
	bck->fd = victim;
    }

    /*
       If a large request, scan through the chunks of current bin to
       find one that fits.  (This will be the smallest that fits unless
       FIRST_SORTED_BIN_SIZE has been changed from default.)  This is
       the only step where an unbounded number of chunks might be
       scanned without doing anything useful with them. However the
       lists tend to be short.
       */

    if (!in_smallbin_range(nb)) {
	bin = bin_at(av, idx);

	for (victim = last(bin); victim != bin; victim = victim->bk) {
	    size = chunksize(victim);

	    if ((unsigned long)(size) >= (unsigned long)(nb)) {
		remainder_size = size - nb;
		unlink(victim, bck, fwd);

		/* Exhaust */
		if (remainder_size < MINSIZE)  {
		    set_inuse_bit_at_offset(victim, size);
		    check_malloced_chunk(victim, nb);
		    retval = chunk2mem(victim);
		    goto DONE;
		}
		/* Split */
		else {
		    remainder = chunk_at_offset(victim, nb);
		    unsorted_chunks(av)->bk = unsorted_chunks(av)->fd = remainder;
		    remainder->bk = remainder->fd = unsorted_chunks(av);
		    set_head(victim, nb | PREV_INUSE);
		    set_head(remainder, remainder_size | PREV_INUSE);
		    set_foot(remainder, remainder_size);
		    check_malloced_chunk(victim, nb);
		    retval = chunk2mem(victim);
		    goto DONE;
		}
	    }
	}
    }

    /*
       Search for a chunk by scanning bins, starting with next largest
       bin. This search is strictly by best-fit; i.e., the smallest
       (with ties going to approximately the least recently used) chunk
       that fits is selected.

       The bitmap avoids needing to check that most blocks are nonempty.
       */

    ++idx;
    bin = bin_at(av,idx);
    block = idx2block(idx);
    map = av->binmap[block];
    bit = idx2bit(idx);

    for (;;) {

	/* Skip rest of block if there are no more set bits in this block.  */
	if (bit > map || bit == 0) {
	    do {
		if (++block >= BINMAPSIZE)  /* out of bins */
		    goto use_unit_top;
	    } while ( (map = av->binmap[block]) == 0);

	    bin = bin_at(av, (block << BINMAPSHIFT));
	    bit = 1;
	}

	/* Advance to bin with set bit. There must be one. */
	while ((bit & map) == 0) {
	    bin = next_bin(bin);
	    bit <<= 1;
	    assert(bit != 0);
	}

	/* Inspect the bin. It is likely to be non-empty */
	victim = last(bin);

	/*  If a false alarm (empty bin), clear the bit. */
	if (victim == bin) {
	    av->binmap[block] = map &= ~bit; /* Write through */
	    bin = next_bin(bin);
	    bit <<= 1;
	}

	else {
	    size = chunksize(victim);

	    /*  We know the first chunk in this bin is big enough to use. */
	    assert((unsigned long)(size) >= (unsigned long)(nb));

	    remainder_size = size - nb;

	    /* unlink */
	    bck = victim->bk;
	    bin->bk = bck;
	    bck->fd = bin;

	    /* Exhaust */
	    if (remainder_size < MINSIZE) {
		set_inuse_bit_at_offset(victim, size);
		check_malloced_chunk(victim, nb);
		retval = chunk2mem(victim);
		goto DONE;
	    }

	    /* Split */
	    else {
		remainder = chunk_at_offset(victim, nb);

		unsorted_chunks(av)->bk = unsorted_chunks(av)->fd = remainder;
		remainder->bk = remainder->fd = unsorted_chunks(av);
		/* advertise as last remainder */
		if (in_smallbin_range(nb))
		    av->last_remainder = remainder;

		set_head(victim, nb | PREV_INUSE);
		set_head(remainder, remainder_size | PREV_INUSE);
		set_foot(remainder, remainder_size);
		check_malloced_chunk(victim, nb);
		retval = chunk2mem(victim);
		goto DONE;
	    }
	}
    }

use_unit_top:
    /*
       If large enough, split off the chunk bordering the end of memory
       (held in av->top). Note that this is in accord with the best-fit
       search rule.  In effect, av->top is treated as larger (and thus
       less well fitting) than any other available chunk since it can
       be extended to be as large as necessary (up to system
       limitations).

       We require that av->top always exists (i.e., has size >=
       MINSIZE) after initialization, so if it would otherwise be
       exhuasted by current request, it is replenished. (The main
       reason for ensuring it exists is that we may need MINSIZE space
       to put in fenceposts in sysmalloc.)
       */
    if ( (&(av->ustate_list))->num != 0) {
	    victim_unit = (&(av->ustate_list))->tail;   
	    do {
		victim = get_ustate(victim_unit)->unit_top;
		size = chunksize(victim);
		if ((unsigned long)(size) >= (unsigned long)(nb)) {
			remainder_size = size - nb;
			remainder = chunk_at_offset(victim, nb);
			//update top_unit information in ustate
			get_ustate(victim_unit)->unit_top = remainder;
			//unlink(victim, bck, fwd);

			/* Exhaust */
			if (remainder_size < MINSIZE)  {
			    set_inuse_bit_at_offset(victim, size);
			}
			/* Split */
			else {
				/* place remainder back to the topbin */		
				//bck = unit_tops(av);
				//fwd = bck->fd;

				//if (fwd != bck) {
				//	/* if smaller than smallest, place first */
				//	if ((unsigned long)(size) < 
				//		(unsigned long)(bck->bk->size)) {
				//		fwd = bck;
				//		bck = bck->bk;
				//	}
				//	else if ((unsigned long)(size) >=
				//		(unsigned long)(MINSIZE)) {

				//		/* maintain topbin in sorted order */
				//		size |= PREV_INUSE; /* Or with inuse bit to speed comparisons */
				//		while ((unsigned long)(size) < (unsigned long)(fwd->size))
				//			fwd = fwd->fd;
				//		bck = fwd->bk;
				//	}
				//}

				//remainder->bk = bck;
				//remainder->fd = fwd;
				//fwd->bk = remainder;
				//bck->fd = remainder;

				set_head(victim, nb | PREV_INUSE);
				set_head(remainder, remainder_size | PREV_INUSE);
				set_foot(remainder, remainder_size);
			}

			check_malloced_chunk(victim, nb);
			retval = chunk2mem(victim);
			goto DONE;
		}
	    } while ( (victim_unit = victim_unit->prev) != NULL );
    }
    //AB_DBG("before call __malloc_alloc\n");
    /* If no space in top, relay to handle system-dependent cases */
    sysmem = __malloc_alloc(pid, nb, av, L);
    retval = sysmem;
DONE:
    __MALLOC_UNLOCK;
    return retval;
}

/* ************** ArbiterThread allocator support *********************** */
/* ----------------------- mstate retrival ----------------------- */

//look up mstate by label, called by ablib_malloc()
static bool _cmp_mstate(const void *key, const void* data)
{
	mstate unit_av = (mstate)data;
	
	if (memcmp(key, &(unit_av->label), sizeof(label_t)) == 0) {
		//AB_DBG("cmp return ture\n");
		return true;
	}
	//AB_DBG("cmp return false\n");
	return false;
}

mstate lookup_mstate_by_label(label_t L)
{
	return (mstate)linked_list_lookup(&(get_abstate()->mstate_list),
					   L, 
					   _cmp_mstate);
}

//locate ustate using memory address, called by ablib_free()
ustate lookup_ustate_by_mem(void *ptr)
{
	struct list_node *victim_unit;
	ustate unit;

	victim_unit = (&(get_abstate()->ustate_list))->tail;
	do {
		unit = get_ustate(victim_unit);
		if ((unsigned long)ptr >= unit->addr && 
			(unsigned long)ptr < unit->addr + unit->length)
			break;
	
	} while ( (victim_unit = victim_unit->prev) != (void *)&(get_abstate()->ustate_list) );

	return unit;
}

//look up mstate by memory address, called by ablib_free()
void lookup_label_by_mem(void *ptr, label_t L)
{
	ustate unit;
	
	unit = (ustate)lookup_ustate_by_mem(ptr);
	memcpy(L, unit->unit_av->label, sizeof(label_t));
}

/* ------------------------ AB_MMAP support ------------------------- */
static unsigned long get_unmapped_area(struct linked_list *list, unsigned long size)
{
	struct list_node *ptr;
	ustate h_unit, l_unit;
	
	if (list->num == 0) {
		//list is empty, no mmapped unit yet
		return (CHANNEL_ADDR + CHANNEL_SIZE - size);
	}
	ptr = list->tail;
	l_unit = ptr->data;
	if (l_unit->addr + l_unit->length + size <= CHANNEL_ADDR + CHANNEL_SIZE) {
		//the hole between the highest unit and channel heap end is available
		return (CHANNEL_ADDR + CHANNEL_SIZE - size);
	}
	else {
		//search for holes among existing unit
		for (ptr = list->tail->prev; ptr != NULL; ptr = ptr->prev) {
			l_unit = ptr->data;
			h_unit = ptr->next->data;
			if (l_unit->addr + l_unit->length + size < h_unit->addr) {
				return (h_unit->addr - size);
			}
		}
		//no holes available, grow downwards
		h_unit = list->head->data;
		if (get_abstate()->ab_top + size <= h_unit->addr) {
			//check if reaching the ab_top
			return (h_unit->addr - size);
		}
	}
	//available are not found
	return 0;
}

static void insert_mmapped_unit(struct linked_list *list, ustate unit)
{
	struct list_node *ptr;
	ustate mmapped_unit;
	
	if (list->num == 0) {
		//list is empty, simply add to list
		list_insert_head(list, unit);
	}
	else if (unit->addr < get_ustate(list->head)->addr){
		//unit area is lower than the existing lowest unit, add to list head
		list_insert_head(list, unit);
	}
	else {
		//unit area is between two existing units
		for (ptr = list->tail; ptr != NULL; ptr = ptr->prev) {
			mmapped_unit = ptr->data;
			if ((mmapped_unit->addr + (unsigned long)(mmapped_unit->length)) < unit->addr) {
				//insert unit
				struct list_node *new = (struct list_node *)malloc(sizeof(struct list_node));
				assert(new);
				new->prev = ptr;
				new->next = ptr->next;
				new->data = unit;
				ptr->next->prev = new;
				ptr->next = new;
				list->num += 1;
				break;
			}
		
		}
		if (ptr == NULL) { //did not insert successfully
			AB_DBG("ERROR: insert_mmapped_unit error");
		}
	}
}
 
/* ----------------------- protection update  ----------------------- */
// update protection for other thread according to label comparision 
static void prot_update(pid_t pid, void *p, long size, label_t L)
{
	/* list_for_each thread
	 * do label check to determine protection
	 * call absys_mprotect() to set protection
	 */
	struct arbiter_thread *abt; 
	struct linked_list *list; 
	struct list_node *ptr;
	struct client_desc *c;
	uint32_t pid_tmp;
	label_t L1;
	own_t O1;
	int prot;
	int pv;
	
	//AB_INFO("prot_update called: arguments = (%d, %p, %ld, %lx, %lx)\n",
	//	pid, p, size, *(unsigned long *)L, (unsigned long)L);
	
	abt = &(arbiter);
	list = &(abt->client_list);
	
	for (ptr = list->head; ptr != NULL; ptr = ptr->next) {
		c= ptr->data;
		
		pid_tmp = c->pid;
		if (pid_tmp == pid) {
			continue;
		}
		*(uint64_t *)L1 = c->label;
		*(uint64_t *)O1 = c->ownership;

		//AB_DBG("before call check_mem_prot: L = %lx, %lx\n", *(uint64_t *)L, (unsigned long)L);
		pv = (int) check_mem_prot(L1, O1, L);
		//AB_DBG("check_mem_prot returns: %d\n", pv);
		switch(pv) {
			case PROT_N: prot = PROT_NONE; break;		
			case PROT_R: prot = PROT_READ; break;		
			case PROT_RW: prot = PROT_READ | PROT_WRITE; break;
			default: prot = PROT_NONE;
		}
		//set protection on page table
		//AB_DBG("absys_mprotect argument=(%d, %lx, %lx, %d)\n)", pid_tmp, p, size, prot);
#ifdef _SYSCALL_COUNT_TIME
		syscall_count[MPROTECT] += 1;
		uint64_t start = rdtsc();
#endif
		absys_mprotect(pid_tmp, p, size, prot);
#ifdef _SYSCALL_COUNT_TIME
		uint64_t end = rdtsc();
		syscall_time[MPROTECT] += end - start;
		printf("SYSCALL MPROTECT: count %d, time %0.2fus\n",
			syscall_count[MPROTECT],
			syscall_time[MPROTECT]/syscall_count[MPROTECT]/_CPU_FRQ);
#endif
	}
}

/* ----------------------- malloc makeup ----------------------- */
//set up existing memory mappings on channel heap for newly created process
void malloc_update(struct client_desc *c_new)
{
	pid_t pid;
	label_t L, L_tmp;
	own_t O;
	struct linked_list *list; 
	struct list_node *ptr;
	ustate unit;
	unsigned long addr;
	size_t length;
	int prot;
	int pv;
	int rc;

	pid = c_new->pid;
	*(uint64_t *)L = c_new->label;
	*(uint64_t *)O = c_new->ownership;
	
	list = &(get_abstate()->ustate_list);

	for (ptr = list->head; ptr != NULL; ptr = ptr->next) {
		unit = ptr->data;
		memcpy(L_tmp, unit->unit_av->label, sizeof(label_t));
		addr = unit->addr;
		length = unit->length;
		
		pv = (int) check_mem_prot(L, O, L_tmp);
		AB_DBG("check_mem_prot returns: %d\n", pv);
		switch(pv) {
			case PROT_N: prot = PROT_NONE; break;		
			case PROT_R: prot = PROT_READ; break;		
			case PROT_RW: prot = PROT_READ | PROT_WRITE; break;
			default: prot = PROT_NONE;
		}
		//set protection on page table
		//AB_MMAP(pid, (void *)addr, length, prot); //TODO: ask Xi: what should absys_mmap do?
#ifdef _SYSCALL_COUNT_TIME
		syscall_count[MPROTECT] += 1;
		uint64_t start = rdtsc();
#endif
		rc = absys_mprotect(pid, (void *)addr, length, prot);
#ifdef _SYSCALL_COUNT_TIME
		uint64_t end = rdtsc();
		syscall_time[MPROTECT] += end - start;
		printf("SYSCALL MPROTECT: count %d, time %0.2fus\n",
			syscall_count[MPROTECT],
			syscall_time[MPROTECT]/syscall_count[MPROTECT]/_CPU_FRQ);
#endif
		if ( rc != 0) {
			AB_MSG("ERROR: malloc_update() failed\n");
		}
		AB_DBG("AB_MMAP argument=(%d, %lx, %d, %d)\n", pid, addr, length, prot);
		AB_DBG("absys_maprotect argument=(%d, %lx, %lx, %d)\n",pid, addr, length, prot);
		//absys_mprotect(pid, addr, length, prot);
				
	}
}

