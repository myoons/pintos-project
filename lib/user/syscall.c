#include <syscall.h>
#include <stdint.h>
#include "../syscall-nr.h"

__attribute__((always_inline))
static __inline int64_t syscall (uint64_t num_, uint64_t a1_, uint64_t a2_,
		uint64_t a3_, uint64_t a4_, uint64_t a5_, uint64_t a6_) {
	int64_t ret;
	register uint64_t *num asm ("rax") = (uint64_t *) num_;
	register uint64_t *a1 asm ("rdi") = (uint64_t *) a1_;
	register uint64_t *a2 asm ("rsi") = (uint64_t *) a2_;
	register uint64_t *a3 asm ("rdx") = (uint64_t *) a3_;
	register uint64_t *a4 asm ("r10") = (uint64_t *) a4_;
	register uint64_t *a5 asm ("r8") = (uint64_t *) a5_;
	register uint64_t *a6 asm ("r9") = (uint64_t *) a6_;

	__asm __volatile(
			"mov %1, %%rax\n"
			"mov %2, %%rdi\n"
			"mov %3, %%rsi\n"
			"mov %4, %%rdx\n"
			"mov %5, %%r10\n"
			"mov %6, %%r8\n"
			"mov %7, %%r9\n"
			"syscall\n"
			: "=a" (ret)
			: "g" (num), "g" (a1), "g" (a2), "g" (a3), "g" (a4), "g" (a5), "g" (a6)
			: "cc", "memory");
	return ret;
}

/* Invokes syscall NUMBER, passing no arguments, and returns the
   return value as an `int'. */
#define syscall0(NUMBER) ( \
		syscall(((uint64_t) NUMBER), 0, 0, 0, 0, 0, 0))

/* Invokes syscall NUMBER, passing argument ARG0, and returns the
   return value as an `int'. */
#define syscall1(NUMBER, ARG0) ( \
		syscall(((uint64_t) NUMBER), \
			((uint64_t) ARG0), 0, 0, 0, 0, 0))
/* Invokes syscall NUMBER, passing arguments ARG0 and ARG1, and
   returns the return value as an `int'. */
#define syscall2(NUMBER, ARG0, ARG1) ( \
		syscall(((uint64_t) NUMBER), \
			((uint64_t) ARG0), \
			((uint64_t) ARG1), \
			0, 0, 0, 0))

#define syscall3(NUMBER, ARG0, ARG1, ARG2) ( \
		syscall(((uint64_t) NUMBER), \
			((uint64_t) ARG0), \
			((uint64_t) ARG1), \
			((uint64_t) ARG2), 0, 0, 0))

#define syscall4(NUMBER, ARG0, ARG1, ARG2, ARG3) ( \
		syscall(((uint64_t *) NUMBER), \
			((uint64_t) ARG0), \
			((uint64_t) ARG1), \
			((uint64_t) ARG2), \
			((uint64_t) ARG3), 0, 0))

#define syscall5(NUMBER, ARG0, ARG1, ARG2, ARG3, ARG4) ( \
		syscall(((uint64_t) NUMBER), \
			((uint64_t) ARG0), \
			((uint64_t) ARG1), \
			((uint64_t) ARG2), \
			((uint64_t) ARG3), \
			((uint64_t) ARG4), \
			0))

struct lock user_lock; 

void
halt (void) {
	syscall0 (SYS_HALT);
	NOT_REACHED ();
}

// Terminates the current user program, returning status to the kernel. 
// If the process's parent waits for it (see below), this is the status that will be returned. 
// Conventionally, a status of 0 indicates success and nonzero values indicate errors.
void
exit (int status) {
	thread_current()->status_exit = status;
	thread_exit();
	// syscall1 (SYS_EXIT, status);
	// NOT_REACHED ();
}

pid_t
fork (const char *thread_name){
	tid_t child_tid;
	struct intr_frame _if;
	lock_acquire (&user_lock);
	_if = thread_current()->tf;
	child_tid = process_fork(thread_name, &_if);
	lock_release (&user_lock);
	return (pid_t)child_tid;
	// return (pid_t) syscall1 (SYS_FORK, thread_name);
}


// Change current process to the executable whose name is given in cmd_line, 
// passing any given arguments. This never returns if successful. 
// Otherwise the process terminates with exit state -1, 
// if the program cannot load or run for any reason.
int
exec (const char *file) {
	int check_return;
	// check user?
	lock_acquire (&user_lock);
	check_return = process_exec(file);
	if (check_return == -1) {
		exit(-1);
	}
	lock_release (&user_lock);
	// return (pid_t) syscall1 (SYS_EXEC, file);
}

int
wait (pid_t pid) {
	return syscall1 (SYS_WAIT, pid);
}

bool
create (const char *file, unsigned initial_size) {
	return syscall2 (SYS_CREATE, file, initial_size);
}


// Deletes the file called file. Returns true if successful, false otherwise. 
bool
remove (const char *file) {
	bool remove_status; 
	check_address((const uint8_t*) file);
	lock_acquire (&user_lock);
  	remove_status = filesys_remove(file);
  	lock_release (&user_lock);

	return remove_status;
	// return syscall1 (SYS_REMOVE, file);
}

int
open (const char *file) {
	return syscall1 (SYS_OPEN, file);
}

// Returns the size, in bytes, of the file open as fd.
// should write code for find_file_fd
int
filesize (int fd) {
	struct file *file;
	int size;
	file = find_file_fd(fd);
	size = file_length(file);

	return size;
	// return syscall1 (SYS_FILESIZE, fd);
}


// Reads size bytes from the file open as fd into buffer. 
// Returns the number of bytes actually read (0 at end of file), 
// or -1 if the file could not be read (due to a condition other than end of file).
// fd 0 reads from the keyboard using input_getc().
int
read (int fd, void *buffer, unsigned size) {
	int read_size;
	check_address((const uint8_t*) buffer);
	check_address((const uint8_t*) buffer + size - 1);

	lock_acquire (&user_lock);
	if (fd == 0){
		for (unsigned i=0; i<size; i++){
			if (!put_user(buffer+i, input_getc()) {
				lock_release(&user_lock);
				sys_exit(-1);
			}
		}
		read_size = size;
	}
	else {
		file = find_file_fd(fd);
		if (file) {
			read_size = file_read(file, buffer, size);
		}
		else {
			read_size = -1;
		}
	}
	lock_release(&user_lock);

	return read_size;

	// return syscall3 (SYS_READ, fd, buffer, size);
}

int
write (int fd, const void *buffer, unsigned size) {
	return syscall3 (SYS_WRITE, fd, buffer, size);
}


// Changes the next byte to be read or written in open file fd to position, 
// expressed in bytes from the beginning of the file
void
seek (int fd, unsigned position) {
	struct file *file;
	lock_acquire (&user_lock);

	// find_file_fd 짜야함
	file = find_file_fd(fd);
	file_Seek(file, position);

	lock_release (&user_lock);
	syscall2 (SYS_SEEK, fd, position);
}

unsigned
tell (int fd) {
	return syscall1 (SYS_TELL, fd);
}

void
close (int fd) {
	syscall1 (SYS_CLOSE, fd);
}

int
dup2 (int oldfd, int newfd){
	return syscall2 (SYS_DUP2, oldfd, newfd);
}

void *
mmap (void *addr, size_t length, int writable, int fd, off_t offset) {
	return (void *) syscall5 (SYS_MMAP, addr, length, writable, fd, offset);
}

void
munmap (void *addr) {
	syscall1 (SYS_MUNMAP, addr);
}

bool
chdir (const char *dir) {
	return syscall1 (SYS_CHDIR, dir);
}

bool
mkdir (const char *dir) {
	return syscall1 (SYS_MKDIR, dir);
}

bool
readdir (int fd, char name[READDIR_MAX_LEN + 1]) {
	return syscall2 (SYS_READDIR, fd, name);
}

bool
isdir (int fd) {
	return syscall1 (SYS_ISDIR, fd);
}

int
inumber (int fd) {
	return syscall1 (SYS_INUMBER, fd);
}

int
symlink (const char* target, const char* linkpath) {
	return syscall2 (SYS_SYMLINK, target, linkpath);
}

int
mount (const char *path, int chan_no, int dev_no) {
	return syscall3 (SYS_MOUNT, path, chan_no, dev_no);
}

int
umount (const char *path) {
	return syscall1 (SYS_UMOUNT, path);
}
