#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "userprog/process.h"
#include "threads/flags.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "intrinsic.h"
#include "vm/vm.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
bool less_fd(const struct list_elem *a, const struct list_elem *b, void *aux);

void is_valid_address (uint64_t* addr);
struct page* get_page_from_address (uint64_t* addr);
void is_valid_buffer(void* buffer, unsigned length, bool writable);
struct struct_fd* get_struct_with_fd (int fd);
int put_fd_with_file (struct file* target_file);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

/* No internal synchronization. Concurrent accesses will interfere with one another.
 * You should use synchronization to ensure that only one process at a time is executing file system code. */
struct lock lock_for_filesys;

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

    /* Initialize lock for file system. */
    lock_init(&lock_for_filesys);
}

void is_valid_address(uint64_t* uaddr)
{
    if (uaddr == NULL || !(is_user_vaddr(uaddr)) || pml4_get_page(thread_current()->pml4, uaddr) == NULL)
        exit(-1);
}

struct page*
get_page_from_address (uint64_t* uaddr) {
    struct page* page;

    if (is_kernel_vaddr(uaddr))
        exit(-1);

    page = spt_find_page(&thread_current()->spt, uaddr);
    return page;
}

void
is_valid_buffer(void* buffer, unsigned length, bool write) {
    struct page* page;

    for (int i=0; i<length; i++) {
        page = get_page_from_address(buffer+i);

        if (write == true && page->writable == false)
            exit(-1);
    }
}

/* Halt the operating system. */
void halt (void) {
    if (lock_held_by_current_thread(&lock_for_filesys))
        lock_release(&lock_for_filesys);

    power_off();

    /* Make sure the thread is exited. */
    NOT_REACHED();
}

/* Terminate this process. */
void exit(int status) {
    if (lock_held_by_current_thread(&lock_for_filesys))
        lock_release(&lock_for_filesys);

    thread_current()->exit_status = status;
    printf("%s: exit(%d)\n", thread_current()->name, thread_current()->exit_status);
    thread_exit();

    /* Make sure the thread is exited. */
    NOT_REACHED();
}

/* Switch current process. */
int exec (const char* file) {
    is_valid_address((uint64_t*) file);

    if (!lock_held_by_current_thread (&lock_for_filesys))
        lock_acquire(&lock_for_filesys);

    /* Copy file name for parsing; It should not affect other jobs using file_name */
    int n;
    char* fn_copy;
    n = strlen(file) + 1;
    fn_copy = (char*) palloc_get_page(PAL_ZERO);
    if (fn_copy == NULL)
        exit(-1);

    strlcpy (fn_copy, file, n);

    int result;
    result = process_exec(fn_copy);

    if (lock_held_by_current_thread(&lock_for_filesys))
        lock_release(&lock_for_filesys);

    if (result == -1) {
        return -1;
    }

    /* Make sure the thread is exited. */
    NOT_REACHED();
    return 0;
}

/* Wait for a child process to die. */
int wait (pid_t pid) {
    if (lock_held_by_current_thread(&lock_for_filesys))
        lock_release(&lock_for_filesys);

    return process_wait(pid);
}

/* Create a file. */
bool create (const char* file, unsigned initial_size) {
    is_valid_address((uint64_t*) file);
    bool result;

    lock_acquire(&lock_for_filesys);
    result = filesys_create(file, initial_size);
    lock_release(&lock_for_filesys);

    return result;
}

/* Delete a file. */
bool remove (const char* file) {
    is_valid_address((uint64_t*) file);
    bool result;


    if (!lock_held_by_current_thread(&lock_for_filesys))
        lock_acquire(&lock_for_filesys);

    result = filesys_remove(file);

    if (lock_held_by_current_thread(&lock_for_filesys))
        lock_release(&lock_for_filesys);

    return result;
}

struct struct_fd* get_struct_with_fd (int fd) {
    struct list_elem* current_list_elem;
    struct struct_fd* to_find_struct_fd;

    if (!list_empty(&(thread_current()->list_struct_fds))) {
        current_list_elem = list_begin(&(thread_current()->list_struct_fds));

        while (current_list_elem != list_end(&(thread_current()->list_struct_fds))) {
            struct struct_fd* target_struct_fd = list_entry (current_list_elem, struct struct_fd, elem);
            if (fd == target_struct_fd->fd) {
                to_find_struct_fd = target_struct_fd;
                break;
            }
            current_list_elem = list_next(current_list_elem);
        }
    }

    return to_find_struct_fd;
}

int put_fd_with_file (struct file* target_file) {
    int stored_fd = 2;
    struct list_elem* current_elem;
    struct thread* curr = thread_current();
    struct struct_fd* target_struct_fd = (struct struct_fd*) malloc(sizeof(struct struct_fd));

    target_struct_fd->file = target_file;
    target_struct_fd->fd = -1;

    if (list_empty(&curr->list_struct_fds))
        target_struct_fd->fd = stored_fd;
    else {
        for (current_elem=list_begin(&curr->list_struct_fds); current_elem != list_end(&curr->list_struct_fds);
             current_elem=list_next(current_elem))
            stored_fd++;

        target_struct_fd->fd = stored_fd;
    }

    list_insert_ordered(&curr->list_struct_fds, &target_struct_fd->elem, less_fd, NULL);
    curr->fds = stored_fd;
    return stored_fd;
}

bool
less_fd(const struct list_elem* first_elem, const struct list_elem* second_elem, void *aux UNUSED){
    int first_fd = list_entry(first_elem, struct struct_fd, elem)->fd;
    int second_fd = list_entry(second_elem, struct struct_fd, elem)->fd;
    return first_fd < second_fd;
}


/* Open a file. */
int open (const char* file) {
    is_valid_address((uint64_t*) file);
    struct file* opened_file;
    int result;

    if (!lock_held_by_current_thread(&lock_for_filesys))
        lock_acquire(&lock_for_filesys);

    opened_file = filesys_open(file);

    if (opened_file == NULL) {
        thread_current()->exit_status = -1;
        result = -1;
    }

    else
        result = put_fd_with_file(opened_file);

    if (lock_held_by_current_thread (&lock_for_filesys))
        lock_release(&lock_for_filesys);

    return result;
}

/* Obtain a file's size. */
int filesize (int fd) {
    struct struct_fd* target_struct_fd;
    int result;

    if (!lock_held_by_current_thread(&lock_for_filesys))
        lock_acquire(&lock_for_filesys);

    target_struct_fd = get_struct_with_fd(fd);

    if (lock_held_by_current_thread(&lock_for_filesys))
        lock_release(&lock_for_filesys);

    if (target_struct_fd == NULL)
        result = -1;
    else
        result = (int) file_length(target_struct_fd->file);

    if (lock_held_by_current_thread(&lock_for_filesys))
        lock_release(&lock_for_filesys);

    return result;
}

/* Read from a file. */
int read (int fd, void* buffer, unsigned length) {
    is_valid_address((uint64_t*) buffer);
    struct struct_fd* target_struct_fd;
    int result;

    if (fd == 0)
        input_getc(buffer, length);
    else if (fd == 1)
        result = -1;
    else {
        if (!lock_held_by_current_thread(&lock_for_filesys))
            lock_acquire(&lock_for_filesys);

        target_struct_fd = get_struct_with_fd(fd);

        if (target_struct_fd == NULL)
            result = -1;
        else
            result = (int) file_read(target_struct_fd->file, buffer, length);

        if (lock_held_by_current_thread(&lock_for_filesys))
            lock_release(&lock_for_filesys);
    }

    return result;
}

/* Write to a file. */
int write (int fd, const void* buffer, unsigned length) {
    is_valid_address((uint64_t*) buffer);
    struct struct_fd* target_struct_fd;
    int result;

    if (fd == 0)
        result = -1;
    else if (fd == 1) {
        putbuf(buffer, length);
        result = length;
    }
    else {
        if (!lock_held_by_current_thread(&lock_for_filesys))
            lock_acquire(&lock_for_filesys);

        target_struct_fd = get_struct_with_fd(fd);

        if (target_struct_fd == NULL)
            result = -1;
        else
            result = (int) file_write(target_struct_fd->file, buffer, length);

        if (lock_held_by_current_thread(&lock_for_filesys))
            lock_release(&lock_for_filesys);
    }

    return result;
}

/* Change position in a file. */
void seek (int fd, unsigned position) {
    struct struct_fd* target_struct_fd;

    if (!lock_held_by_current_thread(&lock_for_filesys))
        lock_acquire(&lock_for_filesys);

    target_struct_fd = get_struct_with_fd(fd);

    if (target_struct_fd != NULL)
        file_seek(target_struct_fd->file, position);

    if (lock_held_by_current_thread(&lock_for_filesys))
        lock_release(&lock_for_filesys);
}

/* Report current position in a file. */
unsigned tell (int fd) {
    struct struct_fd* target_struct_fd;
    int result;

    if (!lock_held_by_current_thread(&lock_for_filesys))
        lock_acquire(&lock_for_filesys);

    target_struct_fd = get_struct_with_fd(fd);

    if (target_struct_fd == NULL)
        result = -1;
    else
        result = (unsigned) file_tell(target_struct_fd->file);

    if (lock_held_by_current_thread(&lock_for_filesys))
        lock_release(&lock_for_filesys);

    return result;
}

/* Close a file. */
void close (int fd) {
    struct struct_fd* target_struct_fd;

    if (!lock_held_by_current_thread(&lock_for_filesys))
        lock_acquire(&lock_for_filesys);

    target_struct_fd = get_struct_with_fd(fd);

    if (target_struct_fd != NULL) {
        file_close(target_struct_fd->file);
        list_remove(&(target_struct_fd->elem));
        free(target_struct_fd);
    }

    if (lock_held_by_current_thread(&lock_for_filesys))
        lock_release(&lock_for_filesys);
}

void *mmap (void *addr, size_t length, int writable, int fd, off_t offset) {

    if (offset % PGSIZE != 0)
        return NULL;


    if (pg_round_down(addr) != addr || is_kernel_vaddr(addr) || addr == NULL || (long long)length <= 0)
        return NULL;

    if (fd == 0 || fd == 1)
        exit(-1);

    // vm_overlap
    if (spt_find_page(&thread_current()->spt, addr))
        return NULL;

    struct file* target = get_struct_with_fd(fd)->file;

    if (target == NULL)
        return NULL;

    void * ret = do_mmap(addr, length, writable, target, offset);

    return ret;
}

void munmap (void *addr) {
    do_munmap(addr);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
    /* Implementation Start */
    uint64_t syscall_number =  f->R.rax;

    switch (syscall_number) {

        case SYS_HALT:
            halt();
        case SYS_EXIT:
            exit(f->R.rdi);
        case SYS_FORK:
            f->R.rax = process_fork(f->R.rdi, f);
            break;
        case SYS_EXEC:
            f->R.rax = exec(f->R.rdi);
            if (f->R.rax == -1)
                exit(-1);
            break;
        case SYS_WAIT:
            f->R.rax = wait(f->R.rdi);
            break;
        case SYS_CREATE:
            f->R.rax = create(f->R.rdi, f->R.rsi);
            break;
        case SYS_REMOVE:
            f->R.rax = remove(f->R.rdi);
            break;
        case SYS_OPEN:
            f->R.rax = open(f->R.rdi);
            break;
        case SYS_FILESIZE:
            f->R.rax = filesize(f->R.rdi);
            break;
        case SYS_READ:
            is_valid_buffer(f->R.rsi, f->R.rdx, 1);
            f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
            break;
        case SYS_WRITE:
            is_valid_buffer(f->R.rsi, f->R.rdx, 0);
            f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
            break;
        case SYS_SEEK:
            seek(f->R.rdi, f->R.rsi);
            break;
        case SYS_TELL:
            tell(f->R.rdi);
            break;
        case SYS_CLOSE:
            close(f->R.rdi);
            break;
        case SYS_MMAP:
            f->R.rax = mmap(f->R.rdi, f->R.rsi, f->R.rdx, f->R.r10, f->R.r8);
            break;
        case SYS_MUNMAP:
            munmap(f->R.rdi);
            break;
        default:
            PANIC("WRONG SYSTEM CALL NUMBER?");
            break;
    }
}
