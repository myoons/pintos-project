/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "vm/inspect.h"

typedef bool (*INITIALIZER)(struct page* , enum vm_type, void*);
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
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* Computes and returns the hash value for hash element E, given
 * auxiliary data AUX. */
static uint64_t hash_get_value (const struct hash_elem* e, void* aux UNUSED);

/* Compares the value of two hash elements A and B, given
 * auxiliary data AUX.  Returns true if A is less than B, or
 * false if A is greater than or equal to B. */
static bool hash_value_less (const struct hash_elem* a, const struct hash_elem* b, void* aux UNUSED);



/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {
	ASSERT (VM_TYPE(type) != VM_UNINIT);

    bool result;
    struct page* new_page;
	struct supplemental_page_table* spt;
    INITIALIZER initializer;

    spt = &thread_current()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {

        new_page = malloc(sizeof(struct page));

        switch (VM_TYPE(type)) {
            case VM_FILE:
                initializer = file_backed_initializer;
                break;
            case VM_ANON:
                initializer = anon_initializer;
                break;
        }

        uninit_new(new_page, upage, init, type, aux, initializer);

        new_page->writable = writable;
        result = spt_insert_page(spt, new_page);
        return result;
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table* spt, void* va) {
    struct page* page;
    struct page* return_page;
    struct hash_elem* target_elem;

    page->va = pg_round_down(va);
    target_elem = hash_find(spt->hash_table, &page->elem_for_hash_table);
    if (target_elem == NULL)
        return NULL;

    return_page = hash_entry(target_elem, struct page, elem_for_hash_table);
    return return_page;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt, struct page *page) {
	int result = false;
    struct hash_elem* target_elem;

    target_elem = hash_insert(spt->hash_table, &page->elem_for_hash_table);
    if (target_elem == NULL)
        result = true;

	return result;
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
	 /* TODO: The policy for eviction is up to you. */
	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame* victim = vm_get_victim ();
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
    new_frame = (struct frame*) malloc(sizeof(struct frame));
    ASSERT (new_frame != NULL);
//    ASSERT (new_frame->page == NULL);

    new_frame->kva = palloc_get_page(PAL_USER);
    if (new_frame->kva == NULL) {  // If memory is full
//        new_frame = vm_evict_frame();
//        new_frame->page = NULL;
        PANIC("TODO");
        return new_frame;
    }

    list_push_back(&frame_list, &new_frame->elem_for_frame_list);

    return new_frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr,
		bool user, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table* spt;
    void* rsp_stack;

    /* TODO: Validate the fault */
    /* TODO: Your code goes here */
    if (is_kernel_vaddr(addr))
        return false;

    spt = &thread_current()->spt;
    rsp_stack = is_kernel_vaddr(f->rsp) ? thread_current()->rsp_stack : f->rsp;
    if (not_present){
        if (!vm_claim_page(addr)) {
            if (rsp_stack - 8 <= addr && USER_STACK - 0x100000 <= addr && addr <= USER_STACK) {
                vm_stack_growth(thread_current()->stack_bottom - PGSIZE);
                return true;
            }
            return false;
        }
        else
            return true;
    }
    return false;
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
    page = spt_find_page(&thread_current()->spt, va);

    if (page == NULL)
        return false;

	return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
    bool result = false;
	struct frame* frame = vm_get_frame();

	/* Set links */
	frame->page = page;
	page->frame = frame;

    result = pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable);
    if (result)
        return swap_in(page, frame->kva);

    return result;
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
    struct hash* new_hash_table = malloc(sizeof(struct hash));  // page_table 메모리에 할당
    hash_init (new_hash_table, hash_get_value, hash_value_less, NULL);  // hast_table 초기화
    spt->hash_table = new_hash_table;  // spt 에 할당, 초기화한 page_table 연결
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

// key 를 가지고 hash table 의 bucket 안의 index 로 변형
static uint64_t hash_get_value (const struct hash_elem* target_elem, void* aux UNUSED)
{
    struct page* target_page = hash_entry(target_elem, struct page, elem_for_hash_table);
    return hash_bytes(&target_page->va, sizeof target_page->va);
}

static bool hash_value_less (const struct hash_elem* first_elem, const struct hash_elem* second_elem, void *aux UNUSED) {
    const struct page* first_page = hash_entry (first_elem, struct page, elem_for_hash_table);
    const struct page* second_page = hash_entry (second_elem, struct page, elem_for_hash_table);
    return first_page->va < second_page->va;
}