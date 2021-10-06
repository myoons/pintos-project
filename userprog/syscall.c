#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

struct lock user_lock;

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
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.

	int syscall_number;

	read_memory(f->rsp, &syscall_number, sizeof(syscall_number));
	thread_current()->rsp_current = f->rsp;

	switch (syscall_number) {
		case SYS_HALT : 
			halt();

		case SYS_EXIT :
		{
			int status;
			read_memory(f->rsp+4, &status, sizeof(status));
			exit(status);
		}

		case SYS_FORK :

		case SYS_EXEC :
		{
			const char* file;
			read_memory(f->rsp + 4, &file, sizeof(file));
			exec(file);
		}

		case SYS_WAIT :

		case SYS_CREATE :

		case SYS_REMOVE :
		{
			const char* filename;
			read_memory(f->rsp + 4, &filename, sizeof(filename));
			remove(filename);
		}

		case SYS_OPEN :

		case SYS_FILESIZE :
		{
			int fd;
			read_memory(f->rsp+4, &fd, sizeof(fd));
			filesize(fd);
		}

		case SYS_READ :
		{
			int fd;
			void *buffer;
			unsigned size;
			read_memory(f->rsp + 4, &fd, sizeof(fd));
			read_memory(f->rsp + 8, &buffer, sizeof(buffer));
			read_memory(f->rsp + 12, &size, sizeof(size));
			read(fd, buffer, size);
		}

		case SYS_WRITE :

		case SYS_SEEK :
		{
			int fd;
			unsigned position;
			read_memory(f->rsp + 4, &fd, sizeof(fd));
			read_memory(f->rsp + 8, &position, sizeof(position));
			seek(fd, position);
		}

		case SYS_TELL :

		case SYS_CLOSE : 


	}

	printf ("system call!\n");
	thread_exit ();
}


/* Reads a byte at user virtual address UADDR.
 * UADDR must be below KERN_BASE.
 * Returns the byte value if successful, -1 if a segfault
 * occurred. */
static int64_t
get_user (const uint8_t *uaddr) {
    int64_t result;

	if ( (void *)uaddr >= KERN_BASE ) {
		return -1;
	}

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

	if ( (void *)udst >= KERN_BASE ) {
    	return false;
  	}

    __asm __volatile (
    "movabsq $done_put, %0\n"
    "movb %b2, %1\n"
    "done_put:\n"
    : "=&a" (error_code), "=m" (*udst) : "q" (byte));
    return error_code != -1;
}


void 
check_address (void *addr) {
	if ( get_user(addr) == -1 ){
		if (lock_held_by_current_thread(&user_lock)) {
			lock_release(&user_lock);
		}
		// exit?? pagefault??
		page_fault();
	}
}


void
read_memory (void *src, void *dst, size_t bytes) {
	for (size_t i =0; i<bytes; i++){
		if (get_user(src + i) == -1) {
			if (lock_held_by_current_thread(&user_lock)) {
				lock_release(&user_lock);
			}
			// exit?? pagefault??
			page_fault();
		}

		*(char*)(dst + i) = get_user(src + i) & 0xff;
	}
}

