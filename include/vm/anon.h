#ifndef VM_ANON_H
#define VM_ANON_H
#include "vm/vm.h"
#include <bitmap.h>
#include "threads/synch.h"
struct page;
enum vm_type;

#define SECTORS_PER_PAGE 8
#define SECTOR_SIZE PGSIZE / SECTORS_PER_PAGE

struct anon_page {
    void *padding;
    enum vm_type type;
    struct page_load_info *aux;
    int swap_table_idx;
};

struct args_swap {
    struct bitmap *swap_table;
    struct lock lock_swap;
};

void vm_anon_init (void);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);

#endif
