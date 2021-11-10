#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

typedef int pid_t;

void syscall_init (void);

struct lock lock_access_file;
struct list open_files;
#endif /* userprog/syscall.h */
