#ifndef _ABT_COMMON_H
#define _ABT_COMMON_H

#include <linux/kernel.h>
#include <linux/mm_types.h>
#include <linux/mm.h>

/********** debug **********/

#define AB_VERBOSE_TAG 0
#define AB_INFO_TAG   0
#define AB_DEBUG_TAG  0

#define AB_MSG(...)  if((AB_VERBOSE_TAG)) {printk(KERN_ERR __VA_ARGS__);}
#define AB_INFO(...) if((AB_INFO_TAG)) {printk(KERN_ERR __VA_ARGS__);}
#define AB_DBG(...)  if((AB_DEBUG_TAG)) {printk(KERN_ERR __VA_ARGS__);}

#define ab_assert(expr)							\
	if (!(expr)) {							\
		printk(KERN_ERR "absys: Assertion failed at %s, %s, %s, line %d\n", \
		       #expr, __FILE__, __func__, __LINE__);		\
		BUG();							\
	}


/*************** abheap **************/
#define AB_HEAP_START 0x80000000


/*********** VMA definitions *********/
#define VM_AB_CONTROL	(VM_SAO)


//check to see if a vma is mapped by the arbiter thread
static inline int is_abt_vma(struct vm_area_struct *vma)
{
	return (vma->vm_flags & VM_AB_CONTROL);
}



extern const struct vm_operations_struct ab_vm_ops;


struct page *abmap_get_page(struct vm_area_struct *vma, unsigned long address);

#endif //_ABT_COMMON_H
