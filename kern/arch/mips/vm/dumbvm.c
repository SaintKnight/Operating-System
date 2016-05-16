/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include "opt-A3.h"

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground.
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

#if OPT_A3
static struct spinlock coremap_lock = SPINLOCK_INITIALIZER;
static int *coremap;
static paddr_t startcont;
static int num;
static int comp = 0;
#endif

void
vm_bootstrap(void)
{
#if OPT_A3

	paddr_t endcont;

	ram_getsize(&startcont, &endcont);
	
	coremap = (int *)PADDR_TO_KVADDR(startcont);

	// total frames available
	num = (endcont - startcont) / PAGE_SIZE;

	// num of frames for core map
	int nfcore = num * 4 / PAGE_SIZE + 1;

	// real content start location
	startcont = startcont + nfcore * PAGE_SIZE;
	num = num - nfcore;

	KASSERT(coremap != NULL);
	// initialize core map
	for(int i = 0; i < num; i++){
		coremap[i] = 0;
	}

	// flag vmboost done
	comp = 1;
#endif
	/* Do nothing. */
}

static
paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;

#if OPT_A3
	if(comp){
		addr = alloc_kpages(npages) - MIPS_KSEG0;
	} else {
		spinlock_acquire(&stealmem_lock);

		addr = ram_stealmem(npages);
	
		spinlock_release(&stealmem_lock);
	}
#else
	spinlock_acquire(&stealmem_lock);

	addr = ram_stealmem(npages);
	
	spinlock_release(&stealmem_lock);
#endif
	return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t 
alloc_kpages(int npages)
{
	paddr_t pa;
#if OPT_A3
	if(comp){
		int is_found = 1;
		if(npages == 0) return 0;

		spinlock_acquire(&coremap_lock);

		KASSERT(coremap != NULL);

		for(int i = 0; i < num; i++){
			for(int j = i; j < i + npages; j++){
				if(coremap[j] != 0){
					is_found = 0;
					break;
				}
			}
			if(is_found){
				coremap[i] = npages;
				for(int j = i + 1; j < i + npages; j++){
					coremap[j] = 1;
				}
				pa = startcont + i * PAGE_SIZE;
				spinlock_release(&coremap_lock);
			
				return PADDR_TO_KVADDR(pa);
			}
			is_found = 1;
		}

		spinlock_release(&coremap_lock);
		return 0;
	}
#endif
	pa = getppages(npages);
	if (pa==0) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
}

void 
free_kpages(vaddr_t addr)
{
	/* nothing - leak the memory. */
#if OPT_A3
	paddr_t pad = addr - MIPS_KSEG0; // later

	spinlock_acquire(&coremap_lock);

	// start from where to free
	int nthframe = (pad - startcont) / PAGE_SIZE;

	// length need to free
	KASSERT(coremap != NULL);

	int x = coremap [(pad - startcont) / PAGE_SIZE];

	// free
	for(int i = nthframe; i < nthframe + x; i++){
		coremap[i] = 0;
	}

	spinlock_release(&coremap_lock);
#else
	(void)addr;
#endif
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

#if OPT_A3

// helper functions for page///////////////////////////////////////////////
unsigned int
get_page_num(vaddr_t vad, vaddr_t vbase){
	return (vad - vbase) / PAGE_SIZE;
}

paddr_t
get_page_offset(vaddr_t vad, vaddr_t vbase){
	return (paddr_t)(vad - vbase) % PAGE_SIZE;
}

unsigned int
get_frame_num(paddr_t pad){
	return (unsigned int)(pad - startcont)/PAGE_SIZE;;
}

paddr_t
get_paddr(unsigned int i){
	return i * PAGE_SIZE + startcont;
}

#endif

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:;
		/* We always create pages read-write, so we can't get this */
		#if OPT_A3
		return EFAULT;
		#else
		panic("dumbvm: got VM_FAULT_READONLY\n");
		#endif
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_npages2 != 0);
#if OPT_A3
#else
	KASSERT(as->as_stackpbase != 0);
#endif
#if OPT_A3
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	//KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	//KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	//KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);
#else
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);
#endif

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	if (faultaddress >= vbase1 && faultaddress < vtop1) {
	#if OPT_A3
		KASSERT(as->as_pbase1 != NULL);

		paddr = get_paddr(as->as_pbase1[get_page_num(faultaddress, as->as_vbase1)]) + 
				get_page_offset(faultaddress, as->as_vbase1);
	#else
		paddr = (faultaddress - vbase1) + as->as_pbase1;
	#endif
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
	#if OPT_A3
		KASSERT(as->as_pbase2 != NULL);

		paddr = get_paddr(as->as_pbase2[get_page_num(faultaddress, as->as_vbase2)]) + 
				get_page_offset(faultaddress, as->as_vbase2);
	#else
		paddr = (faultaddress - vbase2) + as->as_pbase2;
	#endif
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
	#if OPT_A3
		KASSERT(as->as_stackpbase != NULL);

		paddr = get_paddr(as->as_stackpbase[get_page_num(faultaddress, stackbase)]) + 
				get_page_offset(faultaddress, stackbase);
	#else
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
	#endif
	}
	else {
		return EFAULT;
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
#if OPT_A3
		if (faultaddress >= vbase1 && faultaddress < vtop1 && as->flag) {
			elo = paddr | TLBLO_VALID;
		}
#endif
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}
#if OPT_A3
	ehi = faultaddress;
	elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
	if (faultaddress >= vbase1 && faultaddress < vtop1 && as->flag) {
		elo = paddr | TLBLO_VALID;
	}
	tlb_random(ehi, elo);
	splx(spl);
	return 0;
#else
	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
#endif
}

#if OPT_A3

// helper functions for as segpages////////////////////////////////
void
as_set_segpages(size_t s, unsigned int * pb){
	KASSERT(pb != NULL);

	for(size_t i = 0; i < s; i++){
		paddr_t pad = getppages(1);
		unsigned int f = get_frame_num(pad);
		pb[i] = f;
	}
}

void
as_free_segpages(size_t s, unsigned int * pb){
	KASSERT(pb != NULL);

	for(size_t i = 0; i < s; i++){
		free_kpages(PADDR_TO_KVADDR(get_paddr(pb[i])));
	}
}

void
as_copy_segpages(size_t sold, unsigned int * pbnew, unsigned int * pbold){
	KASSERT(pbnew != NULL);
	KASSERT(pbold != NULL);

	for(size_t i = 0; i < sold; i++){
		memmove((void *)PADDR_TO_KVADDR(
			get_paddr(pbnew[i])),
		(const void *)PADDR_TO_KVADDR(
			get_paddr(pbold[i])),
		PAGE_SIZE);
	}
}

#endif

struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}
#if OPT_A3
	as->flag = 0;

	as->as_vbase1 = 0;
	as->as_pbase1 = NULL;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_pbase2 = NULL;
	as->as_npages2 = 0;
	as->as_stackpbase = NULL;
#else
	as->as_vbase1 = 0;
	as->as_pbase1 = 0;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_pbase2 = 0;
	as->as_npages2 = 0;
	as->as_stackpbase = 0;
#endif

	return as;
}

void
as_destroy(struct addrspace *as)
{
#if OPT_A3
	// p1
	as_free_segpages(as->as_npages1, as->as_pbase1);

	// p2
	as_free_segpages(as->as_npages2, as->as_pbase2);

	// stack
	as_free_segpages(DUMBVM_STACKPAGES, as->as_stackpbase);

	kfree(as->as_pbase1);
	kfree(as->as_pbase2);
	kfree(as->as_stackpbase);
#endif
	kfree(as);
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = curproc_getas();
#ifdef UW
        /* Kernel threads don't have an address spaces to activate */
#endif
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/* nothing */
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	size_t npages; 

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	/* We won't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
#if OPT_A3
		as->as_pbase1 = kmalloc(sizeof(int) * npages);
		if(!as->as_pbase1){
			return ENOMEM;
		}

		KASSERT(as->as_pbase1 != NULL);
#endif
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
#if OPT_A3
		as->as_pbase2 = kmalloc(sizeof(int) * npages);
		if(!as->as_pbase2){
			return ENOMEM;
		}

		KASSERT(as->as_pbase2 != NULL);
#endif
		return 0;
	}

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}
#if OPT_A3
#else
static
void
as_zero_region(paddr_t paddr, unsigned npages)
{	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}
#endif

int
as_prepare_load(struct addrspace *as)
{
#if OPT_A3
#else
	KASSERT(as->as_pbase1 == 0);
	KASSERT(as->as_pbase2 == 0);
	KASSERT(as->as_stackpbase == 0);
#endif
#if OPT_A3
	as_set_segpages(as->as_npages1, as->as_pbase1);
	as_set_segpages(as->as_npages2, as->as_pbase2);
#else
	as->as_pbase1 = getppages(as->as_npages1);
	if (as->as_pbase1 == 0) {
		return ENOMEM;
	}

	as->as_pbase2 = getppages(as->as_npages2);
	if (as->as_pbase2 == 0) {
		return ENOMEM;
	}

	as->as_stackpbase = getppages(DUMBVM_STACKPAGES);
	if (as->as_stackpbase == 0) {
		return ENOMEM;
	}
	
	as_zero_region(as->as_pbase1, as->as_npages1);
	as_zero_region(as->as_pbase2, as->as_npages2);
	as_zero_region(as->as_stackpbase, DUMBVM_STACKPAGES);
#endif

	return 0;
}

int
as_complete_load(struct addrspace *as)
{
#if OPT_A3
	as->flag = 1;
	as_activate();
#else
	(void)as;
#endif
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{	
#if OPT_A3
	as->as_stackpbase = kmalloc(sizeof(int) * DUMBVM_STACKPAGES);
	if(!as->as_stackpbase){
		return ENOMEM;
	}

	KASSERT(as->as_stackpbase != 0);

	as_set_segpages(DUMBVM_STACKPAGES, as->as_stackpbase);

#else
	KASSERT(as->as_stackpbase != 0);

#endif
	*stackptr = USERSTACK;
	return 0;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
#if OPT_A3
	new->as_pbase1 = kmalloc(sizeof(int) * new->as_npages1);
	if(!new->as_pbase1){
		return ENOMEM;
	}

	KASSERT(new->as_pbase1 != NULL);
#endif
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;
#if OPT_A3
	new->as_pbase2 = kmalloc(sizeof(int) * new->as_npages2);
	if(!new->as_pbase2){
		return ENOMEM;
	}

	KASSERT(new->as_pbase2 != NULL);
#endif

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

	KASSERT(new->as_pbase1 != 0);
	KASSERT(new->as_pbase2 != 0);

#if OPT_A3
#else
	KASSERT(new->as_stackpbase != 0);
#endif

#if OPT_A3
	new->as_stackpbase = kmalloc(sizeof(int) * DUMBVM_STACKPAGES);
	if(!new->as_stackpbase){
		return ENOMEM;
	}

	KASSERT(new->as_stackpbase != NULL);
	
	as_set_segpages(DUMBVM_STACKPAGES, new->as_stackpbase);

	// p1
	as_copy_segpages(old->as_npages1, new->as_pbase1, old->as_pbase1);

	// p2
	as_copy_segpages(old->as_npages2, new->as_pbase2, old->as_pbase2);

	//stack
	as_copy_segpages(DUMBVM_STACKPAGES, new->as_stackpbase, old->as_stackpbase);

#else

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase1),
		(const void *)PADDR_TO_KVADDR(old->as_pbase1),
		old->as_npages1*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase2),
		(const void *)PADDR_TO_KVADDR(old->as_pbase2),
		old->as_npages2*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase),
		(const void *)PADDR_TO_KVADDR(old->as_stackpbase),
		DUMBVM_STACKPAGES*PAGE_SIZE);
#endif
	*ret = new;
	return 0;
}
