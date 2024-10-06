/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

static struct args_swap anon_args_swap;

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* TODO: Set up the swap_disk. */
	swap_disk = disk_get(1,1);
	anon_args_swap.swap_table = bitmap_create( disk_size(swap_disk) / 8 ); 

	lock_init(&anon_args_swap.lock_swap);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;
	struct anon_page *anon_page = &page->anon;

	anon_page->swap_table_idx = -1;
	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
	size_t idx = anon_page->swap_table_idx;

	if (bitmap_test(anon_args_swap.swap_table, idx) == false){
		PANIC("bitmp test failed for swap_table in anon_swap_in");
	}
	
	for (int i = 0 ; i < SECTORS_PER_PAGE; i++){
		void *addr = page->frame->kva + SECTOR_SIZE * i;
		disk_read(swap_disk, 8*idx+i, addr);
	}

	bitmap_set_multiple(anon_args_swap.swap_table, idx, 1, false);

	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	size_t idx;

	lock_acquire(&anon_args_swap.lock_swap);

	idx = bitmap_scan_and_flip(anon_args_swap.swap_table, 0, 1, false);
	anon_page->swap_table_idx = idx;

	lock_release(&anon_args_swap.lock_swap);

	if (idx == BITMAP_ERROR) {
		PANIC("bitmap error in anon_swap_out\n");
	}

	for (int i = 0 ; i < SECTORS_PER_PAGE; i++) {
		void *addr = page->frame->kva + SECTOR_SIZE * i;
		disk_write(swap_disk, SECTORS_PER_PAGE * idx + i, addr);
	}

	pml4_clear_page(page->owner->pml4, page->va);
	page->frame = NULL;

	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	
	ASSERT(thread_current() == page->owner);

	if (page->frame != NULL){
		free(page->frame);
	}
	
	if (anon_page->aux != NULL){
		free(anon_page->aux);
	}
}
