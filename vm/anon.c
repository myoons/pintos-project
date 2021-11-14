/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "lib/kernel/bitmap.h"
#include "threads/synch.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk* swap_disk;
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

struct bitmap* swap_bitmap;
static struct lock swap_lock;

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
    size_t swap_n_bits;

    /* 1:1 - swap */
	swap_disk = disk_get(1, 1);

    swap_n_bits = disk_size(swap_disk) / (PGSIZE / DISK_SECTOR_SIZE);
    swap_bitmap = bitmap_create(swap_n_bits);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
    /* Set up the handler */
	page->operations = &anon_ops;

	struct anon_page *anon_page = &page->anon;

    lock_init (&swap_lock);
    // sema_init(&anon_page->swap_sema, 0);
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {

    struct anon_page *anon_page = &page->anon;
	int bit = anon_page->swap_bit;

    if (bitmap_test(swap_bitmap, bit) == false) {
        return false;
    }

    if (!lock_held_by_current_thread(&swap_lock))
            lock_acquire (&swap_lock);

    for (int i = 0; i < PGSIZE / DISK_SECTOR_SIZE; ++i) {

        disk_read(swap_disk, bit * (PGSIZE / DISK_SECTOR_SIZE) + i, kva + DISK_SECTOR_SIZE * i);
        
    }

    bitmap_set(swap_bitmap, bit, false);

    if (lock_held_by_current_thread(&swap_lock))
            lock_release (&swap_lock);
    
    // sema_down(&(anon_page->swap_sema));

    return true;
//    int bit;
//    bool result;
//	struct anon_page* anon_page = &page->anon;
//
//    bit = anon_page->swap_bit;
//    result = bitmap_test(swap_bitmap, bit);
//
//    if (result) {
//        for (int i = 0; i < PGSIZE / DISK_SECTOR_SIZE; ++i)
//            disk_read(swap_disk, bit * (PGSIZE / DISK_SECTOR_SIZE) + i, kva + DISK_SECTOR_SIZE * i);
//
//        bitmap_set(swap_bitmap, bit, false);
//    }
//
//    return result;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {

    struct anon_page *anon_page = &page->anon;

    if (!lock_held_by_current_thread(&swap_lock))
            lock_acquire (&swap_lock);

	int bit = bitmap_scan(swap_bitmap, 0, 1, false);

    if (bit == BITMAP_ERROR) {
        return false;
    }

    for (int i = 0; i < PGSIZE / DISK_SECTOR_SIZE; ++i) {

        disk_write(swap_disk, bit * (PGSIZE / DISK_SECTOR_SIZE) + i, page->va + DISK_SECTOR_SIZE * i);

    }

    bitmap_set(swap_bitmap, bit, true);

    // sema_up(&(anon_page->swap_sema));
    pml4_clear_page(thread_current()->pml4, page->va);

    anon_page->swap_bit = bit;

    if (lock_held_by_current_thread(&swap_lock))
            lock_release (&swap_lock);

    return true;
//    int bit;
//    bool result;
//	struct anon_page* anon_page = &page->anon;
//
//    bit = bitmap_scan(swap_bitmap, 0, 1, false);
//    if (bit == BITMAP_ERROR)
//        result = false;
//
//    if (result) {
//        for (int i = 0; i < PGSIZE / DISK_SECTOR_SIZE; ++i)
//            disk_write(swap_disk, bit * PGSIZE / DISK_SECTOR_SIZE + i, page->va + DISK_SECTOR_SIZE * i);
//
//        bitmap_set(swap_bitmap, bit, true);
//        pml4_clear_page(thread_current()->pml4, page->va);
//
//        anon_page->swap_bit = bit;
//    }
//
//    return result;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page* anon_page = &page->anon;
    return;
}
