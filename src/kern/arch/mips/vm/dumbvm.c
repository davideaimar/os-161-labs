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
#include <cpu.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground. You should replace all of this
 * code while doing the VM assignment. In fact, starting in that
 * assignment, this file is not included in your kernel!
 *
 * NOTE: it's been found over the years that students often begin on
 * the VM assignment by copying dumbvm.c and trying to improve it.
 * This is not recommended. dumbvm is (more or less intentionally) not
 * a good design reference. The first recommendation would be: do not
 * look at dumbvm at all. The second recommendation would be: if you
 * do, be sure to review it from the perspective of comparing it to
 * what a VM system is supposed to do, and understanding what corners
 * it's cutting (there are many) and why, and more importantly, how.
 */

/* under dumbvm, always have 72k of user stack */
/* (this must be > 64K so argument blocks of size ARG_MAX will fit) */
#define DUMBVM_STACKPAGES    18

/*
 * Wrap ram_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
static struct spinlock memmap_lock = SPINLOCK_INITIALIZER;
/* 
 *	Map of the memory: 
 *  	- bit 0 is page free/not free
 *  	- bit 1..15 is the slot size if page is the starting one, 0 otherwise
 * 	Access it only with memmap_lock and using the appropriate methods
 * 	max RAM size is 512MB:
 * 		- page size 8192B:  65536 pages 
 * 		- page size 4096B: 131072 pages
 * 		- page size 2048B: 262144 pages
 * 		- page size 1024B: 524288 pages
 * 	Max slot size is set to 2^(16-1) - 1 = 32767 pages
 */
static __u16 *mem_map = NULL;  
static size_t num_frames_total;
static size_t num_frames_allocated;
static size_t num_frames_init_allocated;
static paddr_t firstfree;
static bool use_vm = false;

static size_t tot_allocated_pages = 0; // for debugging
static size_t tot_freed_pages = 0; // for debugging

static bool get_page_free(size_t page_index) {
	KASSERT(page_index < num_frames_total);
	return (mem_map[page_index] & 1);
}

static void set_page_free(size_t page_index, bool free) {
	KASSERT(page_index < num_frames_total);
	free = free == 0 ? 0 : 1; // force to 0 or 1
	mem_map[page_index] = (mem_map[page_index] & ~1) | free;
}

static size_t get_slot_size(size_t page_index) {
	KASSERT(page_index < num_frames_total);
	return (size_t)((mem_map[page_index] & 0xFFFE) >> 1);
}

static void set_slot_size(size_t page_index, size_t size) {
	KASSERT(page_index < num_frames_total);
	KASSERT(size < 0x8000);
	// set bit 1..15 to size
	size = size << 1;
	mem_map[page_index] = (mem_map[page_index] & ~0xFFFE) | size;
}

static bool vm_active(){
	short active;
	spinlock_acquire(&memmap_lock);
	active = use_vm;
	spinlock_release(&memmap_lock);
	return active;
}

void
vm_bootstrap(void) {
	size_t i;
	if (vm_active()) return;
	num_frames_total = ram_getsize() / PAGE_SIZE;
	mem_map = (__u16 *) kmalloc(sizeof(__u16) * num_frames_total);
	if (mem_map == NULL) {
		panic("vm_bootstrap: kmalloc failed\n");
	}
	firstfree = ram_getfirstfree(); // from this moment we can't use ram_stealmem
	num_frames_init_allocated = firstfree / PAGE_SIZE + (firstfree % PAGE_SIZE == 0 ? 0 : 1);
	// initialize the memory map
	for (i = 0; i < num_frames_total; i++) {
		if (i < num_frames_init_allocated) {
			set_page_free(i, false);
			if ( i== 0 )
				set_slot_size(i, num_frames_init_allocated);
			else
				set_slot_size(i, 0);
		} else {
			set_page_free(i, true);
			set_slot_size(i, 0);
		}
	}
	num_frames_allocated = num_frames_init_allocated;
	spinlock_acquire(&memmap_lock);
	use_vm = 1;
	spinlock_release(&memmap_lock);
}

/*
 * Check if we're in a context that can sleep. While most of the
 * operations in dumbvm don't in fact sleep, in a real VM system many
 * of them would. In those, assert that sleeping is ok. This helps
 * avoid the situation where syscall-layer code that works ok with
 * dumbvm starts blowing up during the VM assignment.
 */
static
void
dumbvm_can_sleep(void)
{
	if (CURCPU_EXISTS()) {
		/* must not hold spinlocks */
		KASSERT(curcpu->c_spinlocks == 0);

		/* must not be in an interrupt handler */
		KASSERT(curthread->t_in_interrupt == 0);
	}
}

/*
 * Find the first free page slot of the required size.
 * Returns 0 (reserved address) if no free page slot is found, 
 * otherwise returns the index of the free page slot.
 */ 
static 
size_t 
find_first_free_slot(size_t npages){
	size_t found_index=0, i=0, cnt=0;

	do {
		if (get_slot_size(i) != 0) {
			i += get_slot_size(i);
			found_index = i;
			cnt = 0;
		} else {
			cnt++;
			i++;
		}
	} while (cnt < npages && i < num_frames_total);
	
	if (cnt == npages)
		return found_index;
	return 0;
}

static
paddr_t
getppages(size_t npages) {
	paddr_t addr = 0;
	size_t first_page_index, i;

	if (vm_active()) {
		if (npages > 0 && npages < 32768) {
			// find first free slot
			spinlock_acquire(&memmap_lock);
			first_page_index = find_first_free_slot(npages);
			if (first_page_index == 0){
				spinlock_release(&memmap_lock);
				return 0;
			}
			// mark as used
			for (i = 0; i < npages; i++){
				set_page_free(first_page_index + i, false);
			}
			set_slot_size(first_page_index, npages);
			spinlock_release(&memmap_lock);
			addr = (paddr_t) (first_page_index * PAGE_SIZE);
			num_frames_allocated += npages;
			tot_allocated_pages += npages;
		}
	}
	else {
		spinlock_acquire(&stealmem_lock);
		addr = ram_stealmem(npages);
		spinlock_release(&stealmem_lock);
	}
	return addr;
}

/*
 * free all the page-slot that owns the given page
 */
static 
int 
freeppages(paddr_t addr) {
	size_t slot_index, i, npages;
	// currently there aren't priviledge checks
	// if vm is active, the page is used and the address is valid
	if (vm_active() && get_page_free(addr / PAGE_SIZE) == 0 && addr >= firstfree && addr < num_frames_total * PAGE_SIZE) {
		slot_index = addr / PAGE_SIZE;
		spinlock_acquire(&memmap_lock);
		// go to the first page of the slot
		while (get_slot_size(slot_index) == 0) slot_index--;
		npages = get_slot_size(slot_index);
		for (i = 0; i < npages; i++){
			set_page_free(slot_index + i, true);
		}
		set_slot_size(slot_index, 0);
		spinlock_release(&memmap_lock);
		num_frames_allocated -= npages;
		tot_freed_pages += npages;
		return 0;
	}

	return 1;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t
alloc_kpages(unsigned npages) {
	paddr_t pa;

	dumbvm_can_sleep();
	pa = getppages(npages);
	if (pa==0) {
		return 0;
	}

	return PADDR_TO_KVADDR(pa);
}

void
free_kpages(vaddr_t addr) {
	paddr_t paddr = addr - MIPS_KSEG0;
	freeppages(paddr);
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

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
	    case VM_FAULT_READONLY:
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
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

	as = proc_getas();
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
	KASSERT(as->as_stackpbase != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		paddr = (faultaddress - vbase1) + as->as_pbase1;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		paddr = (faultaddress - vbase2) + as->as_pbase2;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
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
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
}

struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}

	as->as_vbase1 = 0;
	as->as_pbase1 = 0;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_pbase2 = 0;
	as->as_npages2 = 0;
	as->as_stackpbase = 0;

	return as;
}

void
as_destroy(struct addrspace *as)
{
	dumbvm_can_sleep();
	freeppages(as->as_pbase1);
	freeppages(as->as_pbase2);
	freeppages(as->as_stackpbase);
	kfree(as);
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = proc_getas();
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

	dumbvm_can_sleep();

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
		return 0;
	}

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return ENOSYS;
}

static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

int
as_prepare_load(struct addrspace *as)
{
	KASSERT(as->as_pbase1 == 0);
	KASSERT(as->as_pbase2 == 0);
	KASSERT(as->as_stackpbase == 0);

	dumbvm_can_sleep();

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

	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	dumbvm_can_sleep();
	(void)as;
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	KASSERT(as->as_stackpbase != 0);

	*stackptr = USERSTACK;
	return 0;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	dumbvm_can_sleep();

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

	KASSERT(new->as_pbase1 != 0);
	KASSERT(new->as_pbase2 != 0);
	KASSERT(new->as_stackpbase != 0);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase1),
		(const void *)PADDR_TO_KVADDR(old->as_pbase1),
		old->as_npages1*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase2),
		(const void *)PADDR_TO_KVADDR(old->as_pbase2),
		old->as_npages2*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase),
		(const void *)PADDR_TO_KVADDR(old->as_stackpbase),
		DUMBVM_STACKPAGES*PAGE_SIZE);

	*ret = new;
	return 0;
}

void
dumbvm_printstats(void){
	kprintf("dumbvm: page allocator statistics:\n");
	kprintf("dumbvm: %d total pages\n", num_frames_total);
	kprintf("dumbvm: %d pages allocated\n", num_frames_allocated);
	kprintf("dumbvm: %d pages free\n", num_frames_total - num_frames_allocated);
	kprintf("dumbvm: %d pages were allocated before VM bootstrap\n", num_frames_init_allocated);
	kprintf("dumbvm: %d history total allocated paged\n", tot_allocated_pages);
	kprintf("dumbvm: %d history total de-llocated paged\n", tot_freed_pages);
	// calculate the pages for each line to print
	size_t pages_per_line = 8;
	if (num_frames_total > 64) {
		pages_per_line = 16;
	}
	if (num_frames_total > 128) {
		pages_per_line = 32;
	}
	if (num_frames_total > 512) {
		pages_per_line = 64;
	}
	kprintf("Memory map, %d pages per line (0=used page, 1=free page)\n\n\t", pages_per_line);
	for (size_t i = 0; i < num_frames_total/pages_per_line + (num_frames_total % pages_per_line != 0 ? 1 : 0) ; i++)
	{
		for (size_t j = 0; j < pages_per_line; j++)
		{
			if (i*pages_per_line + j < num_frames_total)
			{
				kprintf("%d", get_page_free(i*pages_per_line + j));
			}
			else
			{
				kprintf("/");
			}
		}
		kprintf("\n\t");
	}
	kprintf("\n");
}