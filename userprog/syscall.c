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

void is_valid_address (uint64_t* uaddr);
struct page* get_page_from_address (uint64_t* addr);
void is_valid_buffer(void* buffer, unsigned length, bool writable);
struct file* get_file_with_fd (int fd);
int put_fd_with_file (struct file* target_file);
bool is_valid_mmap(void* addr, size_t length, off_t ofs);
int wait (tid_t tid);

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

void is_valid_address(uint64_t* uaddr) {
    if (is_kernel_vaddr(uaddr))
        exit(-1);
}

struct page*
get_page_from_address (uint64_t* uaddr) {
    struct page* target_page;

    target_page = spt_find_page(&thread_current()->spt, uaddr);
    if (target_page == NULL)
        exit(-1);

    return target_page;
}

void
is_valid_buffer(void* buffer, unsigned length, bool write) {
    struct page* target_page;

    for (int i=0; i<length; i++) {
        target_page = get_page_from_address(buffer+i);

        if (write == true && target_page->writable == false)
            exit(-1);
    }
}

/* Halt the operating system. */
void halt (void) {
    power_off();

    /* Make sure the thread is exited. */
    NOT_REACHED();
}

/* Terminate this process. */
void exit(int status) {
    thread_current()->exit_status = status;
    printf("%s: exit(%d)\n", thread_current()->name, thread_current()->exit_status);
    thread_exit();

    /* Make sure the thread is exited. */
    NOT_REACHED();
}

/* Switch current process. */
int exec (const char* file) {
    is_valid_address((uint64_t*) file);

    /* Copy file name for parsing; It should not affect other jobs using file_name */
    int n;
    char* fn_copy;

    n = strlen(file) + 1;

    fn_copy = palloc_get_page(PAL_ZERO);
    if (fn_copy == NULL)
        exit(-1);

    strlcpy(fn_copy, file, n);

    int result;
    result = process_exec(fn_copy);

    if (result == -1)
        return -1;

    /* Make sure the thread is exited. */
    NOT_REACHED();
    return 0;
}

/* Wait for a child process to die. */
int wait (tid_t tid) {
    return process_wait(tid);
}

/* Create a file. */
bool create (const char* file, unsigned initial_size) {
    bool result;

    if (file == NULL)
        exit(-1);

    result = filesys_create(file, initial_size);
    return result;
}

/* Delete a file. */
bool remove (const char* file) {
    is_valid_address((uint64_t*) file);
    bool result;

    result = filesys_remove(file);

    return result;
}

struct file* get_file_with_fd (int fd) {
    struct file* target_file;

    /* Invalid file descriptor. */
    if (fd >= FD_LIMIT || fd <0)
        return NULL;

    target_file = thread_current()->file_descriptor_table[fd];
    return target_file;
}

int put_fd_with_file (struct file* target_file) {
    struct thread* curr = thread_current();
    struct file** curr_fdt = curr->file_descriptor_table;

    /* Find empty file descriptor index. */
    while(curr->file_descriptor_index < FD_LIMIT && curr_fdt[curr->file_descriptor_index])
        curr->file_descriptor_index++;

    /* If file descriptor table is full. */
    if (curr->file_descriptor_index >= FD_LIMIT)
        return -1;

    curr_fdt[curr->file_descriptor_index] = target_file;
    return curr->file_descriptor_index;
}

/* Open a file. */
int open (const char* file) {
    is_valid_address((uint64_t*) file);
    struct file* opened_file;
    int result;

    /* For open-bad-ptr. */
    if (file == NULL)
        return -1;

    opened_file = filesys_open(file);

    if (opened_file == NULL)
        result = -1;
    else
        result = put_fd_with_file(opened_file);

    if (result == -1 && opened_file != NULL)
        file_close(opened_file);

    return result;
}

/* Obtain a file's size. */
int filesize (int fd) {
    int result;
    struct file* target_file;

    target_file = get_file_with_fd(fd);

    if (target_file == NULL)
        result = -1;
    else
        result = (int) file_length(target_file);

    return result;
}

/* Read from a file. */
int read (int fd, void* buffer, unsigned length) {
    int result;
    struct file* target_file;

    if (fd == 0) {
        input_getc(buffer, length);
        result = length;
    }
    else if (fd == 1)
        result = -1;
    else {
        target_file = get_file_with_fd(fd);

        if (target_file == NULL)
            result = -1;
        else {
            if (!lock_held_by_current_thread(&lock_for_filesys))
                lock_acquire(&lock_for_filesys);

            result = (int) file_read(target_file, buffer, length);

            if (lock_held_by_current_thread(&lock_for_filesys))
                lock_release(&lock_for_filesys);
        }
    }

    return result;
}

/* Write to a file. */
int write (int fd, const void* buffer, unsigned length) {
    int result;
    struct file* target_file;

    if (fd == 0)
        result = -1;
    else if (fd == 1) {
        putbuf(buffer, length);
        result = length;
    }
    else {
        target_file = get_file_with_fd(fd);

        if (target_file == NULL)
            result = -1;
        else {
            if (!lock_held_by_current_thread(&lock_for_filesys))
                lock_acquire(&lock_for_filesys);

            result = (int) file_write(target_file, buffer, length);

            if (lock_held_by_current_thread(&lock_for_filesys))
                lock_release(&lock_for_filesys);
        }
    }

    return result;
}

/* Change position in a file. */
void seek (int fd, unsigned position) {
    struct file* target_file;

    target_file = get_file_with_fd(fd);

    if (target_file == NULL || target_file <= 2)
        return;
    else
        file_seek(target_file, position);
}

/* Report current position in a file. */
unsigned tell (int fd) {
    unsigned result;
    struct file* target_file;

    target_file = get_file_with_fd(fd);

    if (target_file == NULL || target_file <=2)
        return;
    else
        result = (unsigned) file_tell(target_file);

    return result;
}

/* Close a file. */
void close (int fd) {
    struct file* target_file;

    target_file = get_file_with_fd(fd);

    if (target_file == NULL)
        return;

    /* STDIN. */
    if(fd == 0 || target_file == 1)
        thread_current()->n_stdin--;

    /* STDOUT. */
    else if(fd == 1 || target_file == 2)
        thread_current()->n_stdout--;

    /* Invalid file descriptor. */
    if (fd < 0 || fd >= FD_LIMIT)
        return;

    thread_current()->file_descriptor_table[fd] = NULL;

    /* STDIN, STDOUT. */
    if (fd <=1 || target_file <= 2)
        return;

    if (target_file->n_opened != 0) {
        target_file->n_opened--;
        return;
    }

    file_close(target_file);
}

/* Duplicate file descriptor. */
int dup2(int oldfd, int newfd) {
    struct file* target_file;
    struct file** curr_file_descriptor;

    target_file = get_file_with_fd(oldfd);
    if (target_file == NULL)
        return -1;

    if (oldfd == newfd)
        return newfd;

    curr_file_descriptor = thread_current()->file_descriptor_table;

    /* Copy STDIN, STDOUT. */
    if (target_file == 1)
        thread_current()->n_stdin++;
    else if (target_file == 2)
        thread_current()->n_stdout++;
    else
        target_file->n_opened++;

    close(newfd);

    /* Assign duplicated file. */
    curr_file_descriptor[newfd] = target_file;

    return newfd;
}

bool is_valid_mmap(void* addr, size_t length, off_t ofs) {
    bool result = true;

    /* Duplicated page. */
    if (spt_find_page(&thread_current()->spt, addr))
        result = false;

    /* Wrong offset. */
    if (ofs % PGSIZE != 0)
        result = false;

    /* Wrong address. */
    if (addr == NULL || is_kernel_vaddr(addr) || pg_round_down(addr) != addr)
        result = false;

    /* Wrong length. */
    if ((long long) length <= 0)
        result = false;

    return result;
}


void* mmap (void* addr, size_t length, int writable, int fd, off_t ofs) {
    struct file* target_file;

    /* Invalid file descriptor. */
    if (fd < 2)
        exit(-1);

    if (!is_valid_mmap(addr, length, ofs))
        return NULL;

    target_file = get_file_with_fd(fd);

    if (target_file == NULL)
        return NULL;

    return do_mmap(addr, length, writable, target_file, ofs);
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
            f->R.rax = tell(f->R.rdi);
            break;
        case SYS_CLOSE:
            close(f->R.rdi);
            break;
        case SYS_DUP2:
            f->R.rax = dup2(f->R.rdi, f->R.rsi);
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
