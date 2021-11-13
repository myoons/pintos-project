/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/vaddr.h"


static struct list frame_list;
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

// 
// static struct list victim_table;

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

//
static uint64_t hash_func_page(const struct hash_elem *elem, void *aux UNUSED);
static bool
hash_less_page(const struct hash_elem *elem1, const struct hash_elem *elem2, void *aux UNUSED);
void spt_destructor(struct hash_elem *e, void* aux);


/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT);

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		bool (*initializer)(struct page *, enum vm_type, void *);
		struct page* page = malloc (sizeof (struct page));

		if (VM_TYPE(type) == VM_ANON){
			initializer = anon_initializer;
		}
		else if (VM_TYPE(type) == VM_FILE) {
			initializer = file_backed_initializer;
		}
		uninit_new(page, upage, init, type, aux, initializer);
		page->writable = writable;
		// page->vm_type = type;

		/* TODO: Insert the page into the spt. */
		spt_insert_page(spt, page);

		return true;
    	
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	struct page *page = NULL;
	struct page pg;
	/* TODO: Fill this function. */
	pg.va = pg_round_down(va);
	struct hash_elem *elem = hash_find(spt->page_table, &pg.hash_elem);
	if (elem == NULL) 
		return NULL;
	page = hash_entry (elem, struct page, hash_elem);
	// ASSERT( va >= page->va );
	// ASSERT( va < page->va + PGSIZE );
	return page;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	int succ = false;
	/* TODO: Fill this function. */

	struct hash_elem *elem = hash_insert(spt->page_table, &page->hash_elem);
	if (elem == NULL)
		succ = true;
	else	
		succ = false;
	
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
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */
	struct list_elem* current_list_elem;

	if (!list_empty(&frame_list)) {
		current_list_elem = list_begin(&frame_list);

		while (current_list_elem != list_end(&frame_list)) {
			victim = list_entry(current_list_elem, struct frame, frame_elem);
			if (pml4_is_accessed(thread_current()->pml4, victim->page->va)) {
				pml4_set_accessed (thread_current()->pml4, victim->page->va, 0);
				current_list_elem = list_next(current_list_elem);
			}
			else
				return victim;
		}
	}

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	swap_out(victim->page);

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;
	/* TODO: Fill this function. */

	frame = malloc(sizeof (struct frame));
	frame->kva = palloc_get_page (PAL_USER);
	frame->page = NULL;
	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);

	// Add swap case handling
	if (frame->kva == NULL) {
	  free (frame);
	  frame = vm_evict_frame ();
	}

	list_push_back (&frame_list, &frame->frame_elem);
	ASSERT (frame->kva != NULL);
	return frame;
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
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */

	// if (is_kernel_vaddr(addr)) {
    //     return false;
	// }

	//수정하기
	// void *rsp_stack = is_kernel_vaddr(f->rsp) ? thread_current()->rsp : f->rsp;
    // if (not_present){
    //     if (!vm_claim_page(addr)) {
    //         if (rsp_stack - 8 <= addr && USER_STACK - 0x100000 <= addr && addr <= USER_STACK) {
    //             vm_stack_growth(thread_current()->stack_bottom - PGSIZE);
    //             return true;
    //         }
    //         return false;
    //     }
    //     else
    //         return true;
    // }
    // return false;



	// return vm_do_claim_page (page);
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
vm_claim_page (void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: Fill this function */
	struct thread *current_thread = thread_current();
	page = spt_find_page (&current_thread->spt, va);

	if (page == NULL) 
		return false;

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();
	struct thread *current_thread = thread_current();

	ASSERT(frame != NULL)
	ASSERT(page !=NULL)

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
    // list_push_back (&victim_table, &page->victim);

    if (!pml4_set_page(current_thread->pml4, page->va, frame->kva, page->writable))
		return false;


	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	struct hash* page = malloc(sizeof (struct hash));
	hash_init (page, hash_func_page, hash_less_page, NULL);
	spt->page_table = page;
}

static uint64_t
hash_func_page(const struct hash_elem *elem, void *aux UNUSED){
	uint64_t bytes;
	const struct page *pg = hash_entry (elem, struct page, hash_elem);
	bytes = hash_bytes(&pg->va, sizeof pg->va);
	return bytes;
}

static bool
hash_less_page(const struct hash_elem *elem1,
           const struct hash_elem *elem2, void *aux UNUSED) {
	bool less;
	const struct page *pg1 = hash_entry (elem1, struct page, hash_elem);
  	const struct page *pg2 = hash_entry (elem2, struct page, hash_elem);

	less = pg1->va < pg2->va;

  	return less;
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
	//  struct hash_iterator i;
    // hash_first (&i, &src->page_table);
    // while (hash_next (&i)) {	// src의 각각의 페이지를 반복문을 통해 복사
    //     struct page *parent_page = hash_entry (hash_cur (&i), struct page, hash_elem);   // 현재 해시 테이블의 element 리턴
    //     enum vm_type type = page_get_type(parent_page);		// 부모 페이지의 type
    //     void *upage = parent_page->va;						// 부모 페이지의 가상 주소
    //     bool writable = parent_page->writable;				// 부모 페이지의 쓰기 가능 여부
    //     vm_initializer *init = parent_page->uninit.init;	// 부모의 초기화되지 않은 페이지들 할당 위해 
    //     void* aux = parent_page->uninit.aux;

    //     if (parent_page->uninit.type & VM_MARKER_0) {
    //         setup_stack(&thread_current()->tf);
    //     }
    //     else if(parent_page->operations->type == VM_UNINIT) {	// 부모 타입이 uninit인 경우
    //         if(!vm_alloc_page_with_initializer(type, upage, writable, init, aux))
    //             return false;
    //     }
    //     else {
    //         if(!vm_alloc_page(type, upage, writable))
    //             return false;
    //         if(!vm_claim_page(upage))
    //             return false;
    //     }

    //     if (parent_page->operations->type != VM_UNINIT) {   //! UNIT이 아닌 모든 페이지(stack 포함)는 부모의 것을 memcpy
    //         struct page* child_page = spt_find_page(dst, upage);
    //         memcpy(child_page->frame->kva, parent_page->frame->kva, PGSIZE);
    //     }
    // }
    // return true;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	//  ASSERT(1>2);
	struct hash_iterator i;

    hash_first (&i, &spt->page_table);
    while (hash_next (&i)) {
        struct page *page = hash_entry (hash_cur (&i), struct page, hash_elem);

        if (page->operations->type == VM_FILE) {
            do_munmap(page->va);
            // destroy(page);
        }
    }
    hash_destroy(&spt->page_table, spt_destructor);
}

void spt_destructor(struct hash_elem *e, void* aux) {
    const struct page *p = hash_entry(e, struct page, hash_elem);
    free(p);
}