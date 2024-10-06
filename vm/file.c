/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "filesys/file.h" 
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "threads/mmu.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;
	struct page_load_info *aux = page->uninit.aux;

	file_page->file = aux->file;
	file_page->ofs = aux->ofs;
	file_page->read_bytes = aux->read_bytes;
	file_page->zero_bytes = aux->zero_bytes;
	file_page->is_first_page = aux->is_first_page;
	file_page->num_left_page = aux-> num_left_page;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page = &page->file;
	struct page_load_info *aux = (struct page_load_info *) malloc (sizeof(struct page_load_info));
	
	aux->file = file_page->file;
	aux->is_first_page = file_page->is_first_page;
	aux->num_left_page = file_page->num_left_page;
	aux->ofs = file_page->ofs;
	aux->read_bytes = file_page->read_bytes;
	aux->zero_bytes = file_page->zero_bytes;

	return file_lazy_load(page, (void *) aux);
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page = &page->file;
	uint64_t *curr_pml4 = page->owner->pml4;
	
	if (pml4_is_dirty(curr_pml4, page->va)) {
		file_seek(file_page->file, file_page->ofs);
		file_write(file_page->file, page->va, file_page->read_bytes);
		pml4_set_dirty(curr_pml4, page->va, 0);
	} 
	pml4_clear_page(curr_pml4, page->va);
	page->frame = NULL;

	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page = &page->file;
	struct thread *curr = thread_current();

	ASSERT(curr == page->owner);

	if (pml4_is_dirty(curr->pml4, page->va)) {
		file_seek(file_page->file, file_page->ofs);
		file_write(file_page->file, page->va, file_page->read_bytes);
	}

	page->writable = true;
	memset(page->va, 0, PGSIZE);
	hash_delete(&curr->spt.page_map, &page->spt_elem);
	
	if (page->frame) {
		free(page->frame);
	}

	page->frame = NULL;
	page->file.file = NULL;
	page->file.is_first_page = NULL;
	page->file.num_left_page = NULL;
	page->file.ofs = NULL;
	page->file.read_bytes = NULL;
	page->file.zero_bytes = NULL;
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {

	ASSERT(addr != NULL);
	ASSERT(length > 0);
	ASSERT(file != NULL);
	ASSERT(pg_round_down(addr) == addr);

	uint32_t target_read_bytes;
	if (length < file_length(file)) {
		target_read_bytes = length;
	} else {
		target_read_bytes = file_length(file);
	}

	uint32_t zero_bytes = pg_round_up(target_read_bytes) - target_read_bytes;
	int page_cnt = (int) pg_round_up(target_read_bytes) / PGSIZE;

	for (int i = 0; i < page_cnt; i++) {
		if(spt_find_page(&thread_current()->spt, addr + i * PGSIZE) != NULL) {
			return NULL;
		}
	}

	bool is_first = true;
	void *target_page = addr;

	while (target_read_bytes > 0 || zero_bytes > 0) {
		size_t page_reads;
		if (target_read_bytes > PGSIZE) {
			page_reads = PGSIZE;
		} else {
			page_reads = target_read_bytes;
		}

		size_t page_zeros = PGSIZE - page_reads;

		struct page_load_info *aux = (struct page_load_info *) malloc(sizeof(struct page_load_info));
		struct file *reopen_file = file_reopen(file);

		aux->file = reopen_file;
		aux->ofs = offset;
		aux->read_bytes = page_reads;
		aux->zero_bytes = page_zeros;
		aux->is_first_page = is_first;
		aux->num_left_page = page_cnt -1;

		if (is_first) {
			is_first = false;
		}

		if (!vm_alloc_page_with_initializer (VM_FILE, target_page, writable, file_lazy_load, aux)) {
			return NULL;
		}

		page_cnt = page_cnt -1;
		offset = offset + page_reads;
		target_read_bytes -= page_reads;
		zero_bytes -= page_zeros;
		target_page += PGSIZE;
	}

	return addr;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	struct thread *curr = thread_current();
	struct page *first_page = spt_find_page(&curr->spt, addr);
	struct file *file = first_page->file.file;

	int unmap_page_cnt = first_page->file.num_left_page;
	for (int i =0 ; i <= unmap_page_cnt; i++){
		struct page *target = spt_find_page(&curr->spt, addr + i * PGSIZE);
		if (target == NULL){
			PANIC("No page in spt while do_munmap");
		}
		spt_page_destroy(&target->spt_elem, NULL);
	}
	file_close(file);
}

static bool file_lazy_load (struct page *page, void *aux){
	uint8_t *pa = (page->frame)->kva;
	struct page_load_info *args = aux;
	
	uint32_t target_bytes = args->read_bytes;

	file_seek(args->file, args->ofs);
	uint32_t read_bytes = (uint32_t) file_read(args->file, pa, target_bytes);
	
	if (read_bytes != target_bytes){
		palloc_free_page(pa);
		return false;
	} else { 
		memset(pa + target_bytes, 0, args->zero_bytes);
		free(aux);
		return true;
	}
}