/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "userprog/process.h"

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
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
//    return true;
    off_t ofs;
    struct file_aux* faux;
    struct file* target_file;
    size_t should_read_bytes;
    size_t should_zero_bytes;
    size_t actual_read_bytes;

    if (&page->file == NULL)
        return false;

    faux = (struct file_aux*) page->uninit.aux;

    ofs = faux->ofs;
    target_file = faux->file;
    should_read_bytes = faux->read_bytes < PGSIZE ? faux->read_bytes : PGSIZE;
    should_zero_bytes = PGSIZE - should_read_bytes;

    file_seek (target_file, ofs);
    actual_read_bytes = file_read(target_file, page->frame->kva, should_read_bytes);

    if (should_read_bytes != actual_read_bytes)
        return false;

    memset (kva + should_read_bytes, 0, should_zero_bytes);
    return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
//    return true;
    struct thread* curr;
    struct file_aux* faux;

    if (&page->file == NULL)
        return false;

    curr = thread_current();
    faux = (struct file_aux*) page->uninit.aux;

    if (pml4_is_dirty(curr->pml4, page->va)) {
        file_write_at(faux->file, page->va, faux->read_bytes, faux->ofs);
        pml4_set_dirty (curr->pml4, page->va, 0);
    }

    pml4_clear_page(curr->pml4, page->va);
    return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
    void* backup_addr;
    struct file_aux* faux;
    struct file* target_file;
    size_t should_read_bytes;
    size_t should_zero_bytes;

    backup_addr = addr;
    target_file = file_reopen(file);
    should_read_bytes = file_length(file) < length ? file_length(file) : length;
    should_zero_bytes = PGSIZE - (should_read_bytes % PGSIZE);

    /* Like load_segment. */
    while (should_read_bytes > 0 || should_zero_bytes > 0) {
        size_t page_read_bytes = should_read_bytes < PGSIZE ? should_read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        faux = (struct file_aux*) malloc(sizeof(struct file_aux));
        faux->ofs = offset;
        faux->file = target_file;
        faux->read_bytes = page_read_bytes;

        if (!vm_alloc_page_with_initializer (VM_FILE, addr, writable, lazy_load_segment, faux))
            return NULL;

        should_read_bytes -= page_read_bytes;
        should_zero_bytes -= page_zero_bytes;
        addr += PGSIZE;
        offset += page_read_bytes;
    }

    return backup_addr;
}

/* Do the munmap */
void
do_munmap (void *addr) {
    struct file_aux* faux;
    struct page* target_page;
    struct thread* curr = thread_current();

    while (1) {
        target_page = spt_find_page(&curr->spt, addr);
        if (target_page == NULL)
            break;

        faux = (struct file_aux*) target_page->uninit.aux;

        if (pml4_is_dirty(curr->pml4, target_page->va)) {
            file_write_at(faux->file, addr, faux->read_bytes, faux->ofs);
            pml4_set_dirty(curr->pml4, target_page->va, 0);
        }

        pml4_clear_page(curr->pml4, target_page->va);
        addr += PGSIZE;
    }
}
