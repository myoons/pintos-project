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
#include "intrinsic.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

void is_valid_address (uint8_t *uaddr);
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

/* Reads a byte at user virtual address UADDR.
 * UADDR must be below KERN_BASE.
 * Returns the byte value if successful, -1 if a segfault
 * occurred. */
static int64_t
get_user (const uint8_t *uaddr) {
    int64_t result;
    __asm __volatile (
    "movabsq $done_get, %0\n"
    "movzbq %1, %0\n"
    "done_get:\n"
    : "=&a" (result) : "m" (*uaddr));
    return result;
}

/* Writes BYTE to user address UDST.
 * UDST must be below KERN_BASE.
 * Returns true if successful, false if a segfault occurred. */
static bool
put_user (uint8_t *udst, uint8_t byte) {
    int64_t error_code;
    __asm __volatile (
    "movabsq $done_put, %0\n"
    "movb %b2, %1\n"
    "done_put:\n"
    : "=&a" (error_code), "=m" (*udst) : "q" (byte));
    return error_code != -1;
}

/* Is the pointer in user space. */
void
is_valid_address (uint8_t *uaddr) {
    if ( get_user(uaddr) == -1 ){
        if (lock_held_by_current_thread(&lock_for_filesys)) {
            lock_release(&lock_for_filesys);
        }
        exit(-1);
    }
}


void
read_memory (void *src, void *dst, size_t bytes) {
	for (size_t i =0; i<bytes; i++){
		if (get_user(src + i) == -1) {
			if (lock_held_by_current_thread(&lock_for_filesys)) {
				lock_release(&lock_for_filesys);
			}
			exit(-1);
		}

		*(char*)(dst + i) = get_user(src + i) & 0xff;
	}
}

///////


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

/* Clone current process. */
pid_t fork (const char* thread_name) {
    tid_t child_tid;
    struct intr_frame* _if;

    _if = &thread_current()->tf;
	sema_down(&(thread_current()->sema_parent_wait));
	
    child_tid = process_fork(thread_name, _if);

    /* Lock parent thread to wait for child exit. */
    // sema_down(&thread_current()->sema_parent_wait);

    return (pid_t) child_tid;
}


/* Switch current process. */
int exec (const char* file) {
	int check_return;
    is_valid_address((uint8_t*) file);

    lock_acquire(&lock_for_filesys);
    check_return = process_exec(file);
    lock_release(&lock_for_filesys);
	
	if (check_return == -1) {
		exit(-1);
	}

    return check_return;
}


/* Wait for a child process to die. */
int wait (pid_t pid) {
    return process_wait(pid);
}

/* Create a file. */
bool create (const char* file, unsigned initial_size) {
    is_valid_address((uint8_t*) file);
    bool result;

    lock_acquire(&lock_for_filesys);
    result = filesys_create(file, initial_size);
    lock_release(&lock_for_filesys);

    return result;
}

/* Delete a file. */
bool remove (const char* file) {
    is_valid_address((uint8_t*) file);
    bool result;

    lock_acquire(&lock_for_filesys);
    result = filesys_remove(file);
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
    int stored_fd;
    struct thread* curr = thread_current();
    struct struct_fd* target_struct_fd = (struct struct_fd*) malloc(sizeof(struct struct_fd));

    stored_fd = curr->next_fd;
    target_struct_fd->fd = stored_fd;
    increase_fd();

    target_struct_fd->file = target_file;

    list_push_front(&(curr->list_struct_fds), &target_struct_fd->elem);
    return stored_fd;
}

/* Open a file. */
int open (const char* file) {
    is_valid_address((uint8_t*) file);
    struct file* opened_file;
    int result;

    lock_acquire(&lock_for_filesys);
    opened_file = filesys_open(file);

    if (opened_file == NULL)
        result = -1;
    else
        result = put_fd_with_file(opened_file);

    lock_release(&lock_for_filesys);
    return result;
}

/* Obtain a file's size. */
int filesize (int fd) {
    struct struct_fd* target_struct_fd;
    int result;

    lock_acquire(&lock_for_filesys);
    target_struct_fd = get_struct_with_fd(fd);

    if (target_struct_fd == NULL)
        result = -1;
    else
        result = (int) file_length(target_struct_fd->file);

    lock_release(&lock_for_filesys);
    return result;
}

/* Read from a file. */
int read (int fd, void* buffer, unsigned length) {
    is_valid_address((uint8_t*) buffer);
    struct struct_fd* target_struct_fd;
    int result;

    lock_acquire(&lock_for_filesys);

    if (fd == 0)
        result = (int) input_getc();
    else if (fd == 1)
        result = -1;
    else {
        target_struct_fd = get_struct_with_fd(fd);

        if (target_struct_fd == NULL)
            result = -1;
        else
            result = (int) file_read(target_struct_fd->file, buffer, length);
    }

    lock_release(&lock_for_filesys);
    return result;
}

/* Write to a file. */
int write (int fd, const void* buffer, unsigned length) {
    is_valid_address((uint8_t*) buffer);
    struct struct_fd* target_struct_fd;
    int result;

    lock_acquire(&lock_for_filesys);

    if (fd == 0)
        result = -1;
    else if (fd == 1) {
        putbuf(buffer, length);
        result = length;
    }
    else {
        target_struct_fd = get_struct_with_fd(fd);

        if (target_struct_fd == NULL)
            result = -1;
        else
            result = (int) file_write(target_struct_fd->file, buffer, length);
    }

    lock_release(&lock_for_filesys);
    return result;
}

/* Change position in a file. */
void seek (int fd, unsigned position) {
    struct struct_fd* target_struct_fd;

    lock_acquire(&lock_for_filesys);
    target_struct_fd = get_struct_with_fd(fd);

    if (target_struct_fd != NULL)
        file_seek(target_struct_fd->file, position);
    else
        exit(-1);

    lock_release(&lock_for_filesys);
}

/* Report current position in a file. */
unsigned tell (int fd) {
    struct struct_fd* target_struct_fd;
    int result;

    lock_acquire(&lock_for_filesys);
    target_struct_fd = get_struct_with_fd(fd);

    if (target_struct_fd == NULL)
        result = -1;
    else
        result = (unsigned) file_tell(target_struct_fd->file);

    lock_release(&lock_for_filesys);
    return result;
}

/* Close a file. */
void close (int fd) {
    struct struct_fd* target_struct_fd;

    lock_acquire(&lock_for_filesys);
    target_struct_fd = get_struct_with_fd(fd);

    if (target_struct_fd != NULL) {
        file_close(target_struct_fd->file);
        list_remove(&(target_struct_fd->elem));
    } else {
        exit(-1);
    }
    lock_release(&lock_for_filesys);
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
            f->R.rax = fork(f->R.rdi);
            break;
        case SYS_EXEC:
            f->R.rax = exec(f->R.rdi);
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
            f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
            break;
        case SYS_WRITE:
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
        default:
            thread_exit();
            break;
    }
}
