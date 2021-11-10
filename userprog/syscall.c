#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "filesys/directory.h"
#include "filesys/off_t.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
bool compare_fd(const struct list_elem *a, const struct list_elem *b, void *aux);
struct file* fd_to_file(struct thread* curr, int fd);

/* An open file. */
struct file {
    struct inode *inode;        /* File's inode. */
    off_t pos;                  /* Current position. */
    bool deny_write;            /* Has file_deny_write() been called? */
};

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
    lock_init(&lock_access_file);
    list_init(&open_files);
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
syscall_handler (struct intr_frame *f) {
    // TODO: Your implementation goes here.
    /*need to modify(corner case + alpha)*/
    struct thread* current = thread_current();
    struct file* file;
    struct file_fd* ffd;
    struct list_elem* pivot;
    struct dir* dir = dir_open_root();
    struct inode* inode=NULL;
    char* name = current->name;
    int fd=2;
    tid_t tid;
    switch(f->R.rax){
        case SYS_HALT://void halt (void);
            //Halt the oprating system (terminating pintos)
            power_off();
            break;

        case SYS_EXIT://void exit (int status);
            // Terminating this process(current user program), return status to the kernel
            current->exit_status=f->R.rdi;
            printf("%s: exit(%d)\n", current->name, current->exit_status);
            thread_exit();
            break;

        case SYS_FORK://pid_t fork (const char *thread_name);
            // need to modify
            tid = process_fork(f->R.rdi, f);
            f->R.rax=tid;
            break;

        case SYS_EXEC://int exec (const char *cmd_line);
            // lock_acquire(&lock_access_file);
            f->R.rax=process_exec(f->R.rdi);
            // lock_release(&lock_access_file);
            break;

        case SYS_WAIT:
            f->R.rax=process_wait(f->R.rdi);
            break;

        case SYS_CREATE:
            if (is_kernel_vaddr(f->R.rdi)|| f->R.rdi==NULL){
                f->R.rax=false;
                current->exit_status=-1;
                printf("%s: exit(%d)\n", current->name, current->exit_status);
                thread_exit();
                break;
            }
            lock_acquire(&lock_access_file);
            if (dir_lookup(dir, f->R.rdi, &inode)){
                f->R.rax=false;
                lock_release(&lock_access_file);
                break;
            }
            lock_release(&lock_access_file);

            f->R.rax=filesys_create(f->R.rdi, f->R.rsi);
            break;

        case SYS_REMOVE:
            f->R.rax=filesys_remove(f->R.rdi);
            break;

        case SYS_OPEN:
            if (is_kernel_vaddr(f->R.rdi)|| f->R.rdi==NULL || f->R.rdi==""){
                f->R.rax=-1;
                current->exit_status=-1;
                printf("%s: exit(%d)\n", current->name, current->exit_status);
                thread_exit();
                break;
            }
            //if R.rdi is kernel region or NULL, return -1
            lock_acquire(&lock_access_file);
            file=filesys_open(f->R.rdi);


            if (file==NULL){
                f->R.rax=-1;
                current->exit_status=-1;
                lock_release(&lock_access_file);
                break;
            }

            ffd=malloc(sizeof(struct file_fd));
            ffd->file=file;
            ffd->fd=-1;

            if (list_empty(&current->list_struct_fds)){
                ffd->fd=fd;
                // if (strcmp(thread_name(), f->R.rdi)==0){
                // 	file_deny_write(ffd->file);
                // }
            }
            else{
                for (pivot=list_begin(&current->list_struct_fds);pivot!=list_end(&current->list_struct_fds);
                     pivot=list_next(pivot)){
                    if (list_entry(pivot, struct file_fd, fd_elem)->fd>fd){
                        // if (strcmp(thread_name(), f-> R.rdi)==0) file_deny_write(ffd->file);
                        ffd->fd=fd;
                        break;
                    }
                    fd++;
                }
                if (ffd->fd==-1) ffd->fd=fd;
            }

            list_insert_ordered(&current->list_struct_fds, &ffd->fd_elem, compare_fd, NULL);
            f->R.rax=ffd->fd;
            lock_release(&lock_access_file);
            break;

        case SYS_FILESIZE:
            file = fd_to_file(current, f->R.rdi);
            f->R.rax = file_length(file);
            break;

        case SYS_READ:
            check_valid_buffer(f->R.rsi, f->R.rdx, f->rsp, 1);
            if (f->R.rdi==NULL){
                f->R.rax=-1;
                current->exit_status=-1;
                printf("%s: exit(%d)\n", current->name, current->exit_status);
                thread_exit();
                break;
            }

            //STDIN
            if (f->R.rdi==0) input_getc(f->R.rsi, f->R.rdx);
            else if (f->R.rdi>1){
                lock_acquire(&lock_access_file);
                file = fd_to_file(current, f->R.rdi);
                f->R.rax = file_read(file, f->R.rsi, f->R.rdx);//need to implement file descriptor -> access file
                lock_release(&lock_access_file);
            }

            break;

        case SYS_WRITE:
            check_valid_buffer(f->R.rsi, f->R.rdx, f->rsp, 0);
            if (f->R.rdi==NULL){
                f->R.rax=-1;
                current->exit_status=-1;
                printf("%s: exit(%d)\n", current->name, current->exit_status);
                thread_exit();
                break;
            }

            if (f->R.rdi==1) putbuf(f->R.rsi, f->R.rdx);
            else if (f->R.rdi>1){
                lock_acquire(&lock_access_file);
                file = fd_to_file(current, f->R.rdi);
                // if (file -> deny_write){
                // 	file_deny_write(file);
                // 	// f->R.rax=0;
                // 	// lock_release(&lock_access_file);
                // 	// break;
                // 	}
                f->R.rax=file_write(file, f->R.rsi, f->R.rdx);
                lock_release(&lock_access_file);
            }
            break;

        case SYS_SEEK:
            // lock_acquire(&lock_access_file);
            file = fd_to_file(current, f->R.rdi);
            file_seek(file, f->R.rsi);
            // lock_release(&lock_access_file);
            break;

        case SYS_TELL:
            // lock_acquire(&lock_access_file);
            file = fd_to_file(current, f->R.rdi);
            f->R.rax=file_tell(file);
            // lock_release(&lock_access_file);
            break;

        case SYS_CLOSE:
            lock_acquire(&lock_access_file);
            file = fd_to_file(current, f->R.rdi);
            //null => exit -1 add!!!!!
            if (!file==NULL){
                // if(file->deny_write) file_deny_write(file);
                file_close(file); //allow_write

                for (pivot=list_begin(&current->list_struct_fds);pivot!=list_end(&current->list_struct_fds);
                     pivot=list_next(pivot)){
                    ffd=list_entry(pivot, struct file_fd, fd_elem);
                    if (ffd->fd==fd){
                        break;
                    }
                }

                list_remove(&ffd->fd_elem);
                free(ffd);
            }

            lock_release(&lock_access_file);
            break;

        default:
            printf ("system call!\n");
            thread_exit ();
            break;
    }
}

bool
compare_fd(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED){
    int a_fd = list_entry(a, struct file_fd, fd_elem) -> fd;
    int b_fd = list_entry(b, struct file_fd, fd_elem) -> fd;
    return a_fd<b_fd;
}

struct file* fd_to_file(struct thread* curr, int fd){
    struct file_fd* ffd;
    struct file* file=NULL;
    struct list_elem* pivot;
    for (pivot=list_begin(&curr->list_struct_fds);pivot!=list_end(&curr->list_struct_fds);
         pivot=list_next(pivot)){
        ffd=list_entry(pivot, struct file_fd, fd_elem);
        if (ffd->fd==fd){
            file=ffd->file;
            break;
        }
    }
    return file;

}

struct page * check_address(void *addr) {
    if (is_kernel_vaddr(addr))
    {
        thread_current()->exit_status = -1;
        thread_exit();
    }
    return spt_find_page(&thread_current()->spt, addr);
}

void check_valid_buffer(void* buffer, unsigned size, void* rsp, bool to_write) {
    for (int i = 0; i < size; i++) {
        struct page* page = check_address(buffer + i);    // 인자로 받은 buffer부터 buffer + size까지의 크기가 한 페이지의 크기를 넘을수도 있음
        if(page == NULL) {
            thread_current()->exit_status = -1;
            thread_exit();
        }
        if(to_write == true && page->writable == false) {
            thread_current()->exit_status = -1;
            thread_exit();
        }
    }
}
