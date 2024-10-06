#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#include "threads/synch.h"
#include "filesys/off_t.h"

void syscall_init (void);

void check_address (void *addr);
struct lock syscall_lock;
int g_fd_nr;

typedef int pid_t;

struct fd_list_elem {
    int fd;
    struct list_elem elem;
    struct file *file_ptr;
};

void SyS_halt (void);
void SyS_exit (int status);
pid_t SyS_fork (const char *thread_name);
int SyS_exec (const char *cmd_line);
int SyS_wait (pid_t pid);
bool SyS_create (const char *file, unsigned initial_size);
bool SyS_remove (const char *file);
int SyS_open (const char *file);
int SyS_filesize (int fd);
int SyS_read (int fd, void *buffer, unsigned size);
int SyS_write(int fd, const void *buffer, unsigned size);
void SyS_seek (int fd, unsigned position);
unsigned SyS_tell (int fd);
void SyS_close (int fd);

#ifdef VM
void *SyS_mmap (void *addr, size_t length, int writable, int fd, off_t offset);
void SyS_munmap (void *addr);
#endif

#ifdef EFILESYS
bool SyS_chdir (const char *dir);
bool SyS_mkdir (const char *dir);
bool SyS_readdir (int fd, char *name);
bool SyS_isdir (int fd);
int SyS_inumber (int fd);
int SyS_symlink (const char *target, const char *linkpath);
#endif

#endif /* userprog/syscall.h */
