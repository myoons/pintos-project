#ifndef VM_ANON_H
#define VM_ANON_H
#include "vm/vm.h"
#include "threads/synch.h"
struct page;
enum vm_type;

struct anon_page {
    int swap_bit;
    struct lock swap_lock;
    struct semaphore swap_sema;
};

void vm_anon_init (void);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);

#endif
