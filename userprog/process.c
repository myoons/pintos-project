#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
}

extern struct lock lock_for_filesys;

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;
	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;

	strlcpy (fn_copy, file_name, PGSIZE);

    /* Copy file name for parsing; It should not affect other jobs using file_name */
    char user_program[128];
    strlcpy(user_program, file_name, strlen(file_name) + 1);

    /* Parse the command line; First argument is name of user program */
    int pos = 0;
    while (user_program[pos] != ' ' && user_program[pos] != '\0')
        pos ++;
    user_program[pos] = '\0';

    /* Create a new thread to execute FILE_NAME. */
	tid = thread_create (user_program, PRI_DEFAULT, initd, fn_copy);

    if (tid == TID_ERROR)
		palloc_free_page (fn_copy);

	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame* if_) {
    struct thread* curr;
    struct thread* child;
    struct list_elem* current_list_elem;

    curr = thread_current();
    tid_t child_tid;

    curr->user_if = if_;

    /* Create child thread. */
    child_tid = thread_create(name, PRI_DEFAULT, __do_fork, thread_current());

    if (child_tid == TID_ERROR)
        return TID_ERROR;

    if (!list_empty(&(curr->list_child_processes))) {
        current_list_elem = list_begin(&(curr->list_child_processes));

        while (current_list_elem != list_end(&(curr->list_child_processes))) {
            struct thread* target_thread = list_entry (current_list_elem, struct thread, elem_for_child);
            if (child_tid == target_thread->tid) {
                child = target_thread;
                break;
            }
            current_list_elem = list_next(current_list_elem);
        }
    }

    /* Wait for child process to finish. */
    sema_down(&child->sema_for_fork);

    if (child->exit_status == -1)
        return TID_ERROR;

    return child_tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread* current = thread_current ();
	struct thread* parent = (struct thread *) aux;
	void* parent_page;
	void* newpage;
	bool writable;

    if (is_kernel_vaddr(va))
        return true;

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);
    if (parent_page == NULL)
        return false;

    newpage = palloc_get_page(PAL_USER);
    if (newpage == NULL)
        return false;

    memcpy(newpage, parent_page, PGSIZE);
    writable = is_writable(pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
        palloc_free_page(newpage);
        return false;
	}

	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *parent = (struct thread *) aux;
	struct thread *current = thread_current ();
	bool succ = true;

    /* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
    struct intr_frame* parent_if;
    parent_if = parent->user_if;

    /* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));

    /* Set return fale. */
    if_.R.rax = 0;

    /* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif
    bool is_exist;
    struct file* copy_file;
    struct file* target_file;
    struct list_elem* current_list_elem;

    // TODO : Condition of setting is_exist TRUE
    for (int i = 0; i < FD_LIMIT; i++) {
        target_file = parent->file_descriptor_table[i];

        if (target_file == NULL)
            continue;

        is_exist = false;
        if (!is_exist) {
            /* STDIN, STDOUT = 999999. */
            if (target_file != 999999)
                copy_file = file_duplicate(target_file);
            else
                copy_file = target_file;

            current->file_descriptor_table[i] = copy_file;
        }
    }

    current->file_descriptor_index = parent->file_descriptor_index;
    sema_up(&(current->sema_for_fork));

	/* Finally, switch to the newly created process. */
    if (succ)
        do_iret(&if_);

error:
    current->exit_status = TID_ERROR;
    sema_up(&current->sema_for_fork);
    exit(TID_ERROR);
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
    char *file_name = f_name;
	bool success;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

    /* We first kill the current context */
    process_cleanup ();

#ifdef VM
    /* Should initialize supplemental page table. */
    supplemental_page_table_init(&thread_current()->spt);
#endif

	/* And then load the binary */
	success = load (file_name, &_if);

    /* If load failed, quit. */
    if (!success) {
        palloc_free_page (file_name);
        return -1;
    }

    palloc_free_page (file_name);

    if (lock_held_by_current_thread(&lock_for_filesys))
        lock_release(&lock_for_filesys);

    /* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid) {
    bool is_my_child;
    struct list_elem* current_list_elem;
    struct thread* child_thread_to_wait;

    is_my_child = false;
    if (!list_empty(&(thread_current()->list_child_processes))) {
        current_list_elem = list_begin(&(thread_current()->list_child_processes));

        while (current_list_elem != list_end(&(thread_current()->list_child_processes))) {
            struct thread* target_thread = list_entry (current_list_elem, struct thread, elem_for_child);
            if (child_tid == target_thread->tid) {
                child_thread_to_wait = target_thread;
                is_my_child = true;
                break;
            }
            current_list_elem = list_next(current_list_elem);
        }
    }

    if (!is_my_child)
        return -1;

    sema_down(&(child_thread_to_wait->sema_for_wait));
    list_remove(&(child_thread_to_wait->elem_for_child));
    sema_up(&(child_thread_to_wait->sema_for_free));
    return child_thread_to_wait->exit_status;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */

    for (int i = 0; i < FD_LIMIT; i++)
        close(i);

    palloc_free_multiple(thread_current()->file_descriptor_table, N_FDT);

    /* Close currently executing file. */
    file_close(thread_current()->curr_exec_file);

    /* Clean up. */
    process_cleanup();

    thread_current()->tf.R.rax=thread_current()->exit_status;

    /* Process change. */
    sema_up(&(thread_current()->sema_for_wait));
    sema_down(&(thread_current()->sema_for_free));
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
    /* Returns true if H contains no elements, false otherwise. */
    if(!hash_empty(&curr->spt.hash_table))
        supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
    struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

    /* Copy file name for parsing; It should not affect other jobs using file_name */
    char user_program[128];
    strlcpy(user_program, file_name, strlen(file_name) + 1);

    /* Parse the command line; First argument is name of user program */
    int pos = 0;
    while (user_program[pos] != ' ' && user_program[pos] != '\0')
        pos ++;
    user_program[pos] = '\0';

    /* Open executable file. */
	file = filesys_open (user_program);
	if (file == NULL) {
		printf ("load: %s: open failed\n", user_program);
		goto done;
	}

    thread_current()->curr_exec_file = file;
    file_deny_write(file);

    /* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;

		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))  // exec-once ERROR POINT
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

    /* Address of stack pointer. */
    void** rsp = &(*if_).rsp;
    push_arguments(file_name, rsp, if_);
    success = true;
done:
    /* We arrive here whether the load is successful or not. */
    return success;
}


void push_arguments(char* cmd_line, void** rsp, struct intr_frame *if_) {

    /* Copy file name for parsing; It should not affect other jobs using cmd_line. */
    char cmd_line_copy[128];
    strlcpy(cmd_line_copy, cmd_line, strlen(cmd_line) + 1);

    /* Parse command line to count number of arguments and get. */
    char *arg;
    char *save_ptr;

    int argc = 0;
    arg = strtok_r(cmd_line_copy, " ", &save_ptr);
    while (arg != NULL) {
        argc++;
        arg = strtok_r(NULL, " ", &save_ptr);
    }

    strlcpy(cmd_line_copy, cmd_line, strlen(cmd_line) + 1);

    int pos = -1;
    arg = strtok_r(cmd_line_copy, " ", &save_ptr);
    char** argv = (char **) malloc(sizeof(char *) * argc);
    while (arg != NULL) {
        pos++;

        /* Store the argument. */
        argv[pos] = arg;
        /* Next argument. */
        arg = strtok_r(NULL, " ", &save_ptr);
    }

    int arg_len;
    int args_total_length = 0;
    char** argv_address = (char **) malloc(sizeof(char *) * argc);
    while (pos >=0) {
        /* Target argument. */
        arg = argv[pos];

        /* Length of argument with \0. */
        arg_len = strlen(arg) + 1;

        /* Calculate total length of arguments to align the stack. */
        args_total_length += arg_len;

        /* Make space to store argument. */
        *rsp = (char*) *rsp - arg_len;

        /* Push argument. */
        strlcpy((char*) *rsp, arg, arg_len);

        /* Store the address of argument. */
        argv_address[pos] = (char*) *rsp;

        pos--;
    }

    /* Push word align. */
    int remain = args_total_length % 8;
    if (remain > 0)
        *rsp = (char*) *rsp - (8 - remain);

    /* Push NULL pointer. */
    *rsp = (uintptr_t*) *rsp - 1;
    **(uint64_t**) rsp = NULL;

    /* Push address of arguments. */
    while ((argc + pos) >= 0) {
        /* Address of target argument. */
        arg = argv_address[argc + pos];

        /* Make space to store address of argument. */
        *rsp = (uintptr_t*) *rsp - 1;

        /* Push argument. */
        **(uint64_t **) rsp = arg;

        /* Next argument. */
        pos--;
    }

    /* Push return address. */
    *rsp = (uintptr_t*) *rsp - 1;
    **(uint64_t**) rsp = 0;

    /* Push rdi & rsi. */
    (*if_).R.rdi = argc;
    (*if_).R.rsi = (uintptr_t*) *rsp + 1;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;  // 0x47480000
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

bool
lazy_load_segment (struct page *page, void *aux) {
    off_t ofs;
    struct file* file;
    struct file_aux* faux;
    size_t should_read_bytes;
    size_t should_zero_bytes;
    size_t actual_read_bytes;

    faux = (struct file_aux*) aux;

    ofs = faux->ofs;
    file = faux->file;
    should_read_bytes = faux->read_bytes < PGSIZE ? faux->read_bytes : PGSIZE;
    should_zero_bytes = PGSIZE - should_read_bytes;

    /* Change current position in FILE. */
    file_seek(file, ofs);

    /* Returns the number of bytes actually read. */
    actual_read_bytes = file_read(file, page->frame->kva, should_read_bytes);

    if (actual_read_bytes != should_read_bytes) {
        palloc_free_page(page->frame->kva);
        return false;
    }

    memset(page->frame->kva + should_read_bytes, 0, should_zero_bytes);
    return true;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

    struct file_aux* faux;
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

        faux = (struct file_aux*) malloc(sizeof(struct file_aux));
		faux->ofs = ofs;
		faux->file = file;
		faux->read_bytes = page_read_bytes;

		if (!vm_alloc_page_with_initializer (VM_ANON, upage, writable, lazy_load_segment, faux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
        ofs += page_read_bytes;
	}

	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
    struct thread* curr = thread_current();

    /* Allocate page. (Type, Upage, writable) */
    if (vm_alloc_page(VM_ANON | VM_MARKER_0, ((uint8_t *) USER_STACK) - PGSIZE, 1))
        success = vm_claim_page(((uint8_t *) USER_STACK) - PGSIZE);

    if (success) {
        curr->stack_pointer = ((uint8_t *) USER_STACK) - PGSIZE;
        if_->rsp = USER_STACK;
    }

	return success;
}
#endif /* VM */
