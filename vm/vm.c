/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"


static uint64_t get_value_from_hash_table (const struct hash_elem* target_elem, void* aux UNUSED);
static bool compare_hash_value (const struct hash_elem* first_elem, const struct hash_elem* second_elem, void* aux UNUSED);

struct list frame_list;

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */

    /* Initialize list of frames. */
    list_init(&frame_list);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame* vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame* vm_evict_frame (void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void* upage, bool writable,
		vm_initializer* init, void* aux) {

    ASSERT (VM_TYPE(type) != VM_UNINIT)

    struct page* new_page;
    struct supplemental_page_table* spt = &thread_current()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page(spt, upage) == NULL) {
        new_page = (struct page*) malloc(sizeof(struct page));

        switch (VM_TYPE(type)) {
            case VM_ANON:
                uninit_new(new_page, upage, init, type, aux, anon_initializer);
                break;
            case VM_FILE:
                uninit_new(new_page, upage, init, type, aux, file_backed_initializer);
                break;
        }

        new_page->writable = writable;
        spt_insert_page(spt, new_page);
        return true;
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page*
spt_find_page (struct supplemental_page_table* spt, void* va) {
    struct page* dummy_page;
    struct page* target_page;
    struct hash_elem* target_hash_elem;

    /* Allocate memory for dummy page (Just for finding target hash element. */
    dummy_page = (struct page*) malloc(sizeof(struct page));

    /* Start point of the va which is offset 0*/
    dummy_page->va = pg_round_down(va);

    /* Find the target hash element of target va. */
    target_hash_elem = hash_find(spt->hash_table, &(dummy_page->elem_for_hash_table));

    if (target_hash_elem == NULL) {
        return NULL;
    }

    /* Get target page using target hash element. */
    target_page = hash_entry(target_hash_elem, struct page, elem_for_hash_table);
    return target_page;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt, struct page *page) {
	int succ = true;
    struct hash_elem* return_hash_elem;

    /* Inserts new page to hash table using hash_elem.
     * Returns hash_elem if the corresponding page already exists, otherwise NULL. */
    return_hash_elem = hash_insert (spt->hash_table, &(page->elem_for_hash_table));

    /* If return value is NULL, successfully inserted. */
    if (return_hash_elem != NULL) {
        succ = false;
    }
	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame* victim;
    struct list_elem* current_list_elem;

     if (!list_empty(&frame_list)) {
         current_list_elem = list_begin(&frame_list);

         while (current_list_elem != list_end(&frame_list)) {
             victim = list_entry(current_list_elem, struct frame, elem_for_frame_list);
             if (pml4_is_accessed(thread_current()->pml4, victim->page->va))
                 pml4_set_accessed (thread_current()->pml4, victim->page->va, 0);
             else
                 break;

             current_list_elem = list_next(current_list_elem);
         }
     }

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim = vm_get_victim ();

    /* Swap out the page. */
    swap_out(victim->page);
	return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame* new_frame = NULL;
	/* TODO: Fill this function. */

    /* Allocate memory for new frame. */
    new_frame = (struct frame*) malloc(sizeof(struct frame));
    ASSERT (new_frame != NULL);

    /* Get new page from user pool. */
    new_frame->kva = palloc_get_page(PAL_USER);

    /* If user pool is full, it returns null pointer. Should evict and fetch. */
    if (new_frame->kva == NULL)
        /* Evict a frame from full user pool. */
        new_frame = vm_evict_frame();


    /* Insert to frame list. */
    list_push_back(&frame_list, &(new_frame->elem_for_frame_list));

    /* Make page NULL so that it could connect to new virtual page. */
    new_frame->page = NULL;
	ASSERT (new_frame->page == NULL);
	return new_frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr) {
    if (vm_alloc_page(VM_ANON | VM_MARKER_0, addr, 1))
    {
        vm_claim_page(addr);
        thread_current()->stack_bottom -= PGSIZE;
    }
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame* f, void *addr,
		bool user UNUSED, bool write UNUSED, bool not_present) {
    void* thread_rsp;

    /* Virtual address should be in user pool. */
    if (is_kernel_vaddr(addr) && user)
        return false;

    if (is_kernel_vaddr(f->rsp))
        thread_rsp = thread_current()->rsp;
    else
        thread_rsp = f->rsp;

    if (thread_rsp - 8 <= addr && USER_STACK - 0x100000 <= addr && addr <= USER_STACK)
        vm_stack_growth(thread_current()->stack_bottom - PGSIZE);

    return vm_claim_page(addr);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va) {
	struct page* page;
    page = spt_find_page(&(thread_current()->spt), va);  // Error because this returns NULL

    /* Error */
    if (page == NULL)
        return false;

	return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page* page) {
    bool result;
    /* Claim the page that allocate on VA. */
    struct frame* frame = vm_get_frame();

    ASSERT (page != NULL);
    ASSERT (frame != NULL);

	/* Set links */
	frame->page = page;
	page->frame = frame;

    result = pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable);

    if (result)
        result = swap_in(page, frame->kva);

    return result;
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
    struct hash* hash_table = (struct hash*) malloc(sizeof(struct hash));

    /* Initialize hash table used in supplemental page table. */
    hash_init(hash_table, get_value_from_hash_table, compare_hash_value, NULL);

    spt->hash_table = hash_table;
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
}

static uint64_t
get_value_from_hash_table (const struct hash_elem* target_elem, void* aux UNUSED)
{
    struct page* target_page = hash_entry(target_elem, struct page, elem_for_hash_table);
    return hash_bytes(&(target_page->va), sizeof target_page->va);
}

static bool
compare_hash_value (const struct hash_elem* first_elem, const struct hash_elem* second_elem, void *aux UNUSED) {
    const struct page* first_page = hash_entry(first_elem, struct page, elem_for_hash_table);
    const struct page* second_page = hash_entry(second_elem, struct page, elem_for_hash_table);
    return first_page->va < second_page->va;
}