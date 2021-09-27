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
    /* Implementation Start */
    uint64_t syscall_number =  f->R.rax;

    switch (syscall_number) {

        case SYS_HALT:
            printf ("SYS_HALT!\n");
            break;
        case SYS_EXIT:
            printf ("SYS_EXIT!\n");
            break;
        case SYS_FORK:
            printf ("SYS_FORK!\n");
            break;
        case SYS_EXEC:
            printf ("SYS_EXEC!\n");
            break;
        case SYS_WAIT:
            printf ("SYS_WAIT!\n");
            break;
        case SYS_CREATE:
            printf ("SYS_CREATE!\n");
            break;
        case SYS_REMOVE:
            printf ("SYS_REMOVE!\n");
            break;
        case SYS_OPEN:
            printf ("SYS_OPEN!\n");
            break;
        case SYS_FILESIZE:
            printf ("SYS_FILESIZE!\n");
            break;
        case SYS_READ:
            printf ("SYS_READ!\n");
            break;
        case SYS_WRITE:
            printf ("SYS_WRITE!\n");
            break;
        case SYS_SEEK:
            printf ("SYS_SEEK!\n");
            break;
        case SYS_TELL:
            printf ("SYS_TELL!\n");
            break;
        case SYS_CLOSE:
            printf ("SYS_CLOSE!\n");
            break;

    }
    /* Implementation End */
	thread_exit ();
}
