// SPDX-License-Identifier: GPL-2.0-or-later
// The Linux x86-64 runtime: what its files share. A title is one statically linked ELF; it reaches the system
// only through the `syscall` instruction (syscall.c), and what it finds there is this folder.
#pragma once
#include "rt.h"

// errno, as the kernel returns it (negated)
enum { L_EPERM = 1, L_ENOENT = 2, L_EINTR = 4, L_EIO = 5, L_EBADF = 9, L_ECHILD = 10, L_EAGAIN = 11, L_ENOMEM = 12, L_EACCES = 13, L_EFAULT = 14,
       L_EEXIST = 17, L_ENOTDIR = 20, L_EISDIR = 21, L_EINVAL = 22, L_EMFILE = 24, L_ENOTTY = 25, L_ESPIPE = 29, L_EROFS = 30, L_ERANGE = 34,
       L_ENAMETOOLONG = 36, L_ENOSYS = 38, L_ELOOP = 40, L_EAFNOSUPPORT = 97, L_ETIMEDOUT = 110 };

// ---- the machine a title is told it runs on (proc.c) ----
#define LINUX_RELEASE "6.1.0-pisco"
#define LINUX_VERSION "#1 SMP PREEMPT_DYNAMIC pisco"
#define LINUX_HOSTNAME "pisco"
#define LINUX_USER "player"
#define LINUX_UID 1000
#define LINUX_MEM_TOTAL (4096ull << 20)   // the module's memory limit (--max-memory)
u64 linux_mem_used(void);                 // what the module's memory has grown to
extern int linux_tty;                     // SW_TTY=1: the standard streams are a terminal (80 columns by 24 rows, or SW_TTY=<columns>x<rows>)
extern u16 linux_cols, linux_rows;
extern char linux_exe[];                  // the title's path as it sees it: "/<file name>", or where its image puts it
extern int linux_drive;                   // the root is a title image's drive (the host knows its files as "c:/..."), not the title's folder
extern int linux_argc; extern char **linux_argv;   // its arguments (argv[0] is linux_exe)
extern const char **linux_env;            // its environment
extern u64 linux_image_end;               // where the image ends (the program break never moves from there)

// ---- files (fs.c): absolute paths, read-only ----
enum { N_FILE = 1, N_DIR, N_LINK, N_NULL, N_RANDOM, N_TTY };
typedef struct {
  int type; u64 size; u64 ino;
  char *data;        // a generated file's bytes or a link's target (malloc'd: the caller frees), or 0: a file of the title's folder
  char host[512];    // its name for the host
} Node;
int fs_path(s64 dirfd, const char *path, char *out, size_t cap);   // the absolute, normal form; 0 or -errno
int fs_lookup(const char *abs, Node *n, int follow);                // 0 or -errno
char *fs_list(const char *abs, u32 *len);                           // "name\n" lines, a directory's end in '/'; 0: no directory
int fs_generated(const char *abs, Node *n);                         // proc.c: the files the runtime makes up; 1 when it is one
void fs_generated_list(const char *abs, char **out, u32 *len);      // proc.c: their names under a directory, added to `out`
int fs_generated_dir(const char *abs);                              // proc.c: a directory that holds some of them

// ---- system calls (syscall.c dispatches; each returns the result or -errno) ----
s64 sys_brk(u64 to); s64 sys_mmap(u64 addr, u64 len, u64 prot, u64 flags, s64 fd, u64 off); s64 sys_munmap(u64 addr, u64 len);
s64 sys_mremap(u64 addr, u64 old, u64 len, u64 flags, u64 to);
s64 sys_openat(s64 dirfd, const char *path, u64 flags); s64 sys_close(s64 fd);
s64 sys_read(s64 fd, void *buf, u64 n); s64 sys_write(s64 fd, const void *buf, u64 n); s64 sys_pread(s64 fd, void *buf, u64 n, u64 off);
s64 sys_readv(s64 fd, u64 iov, u64 n); s64 sys_writev(s64 fd, u64 iov, u64 n); s64 sys_lseek(s64 fd, s64 off, u64 whence);
s64 sys_fstat(s64 fd, u64 st); s64 sys_fstatat(s64 dirfd, const char *path, u64 st, u64 flags);
s64 sys_statx(s64 dirfd, const char *path, u64 flags, u64 mask, u64 stx);
s64 sys_getdents64(s64 fd, u64 buf, u64 n); s64 sys_readlinkat(s64 dirfd, const char *path, char *buf, u64 n);
s64 sys_faccessat(s64 dirfd, const char *path, u64 mode); s64 sys_ioctl(s64 fd, u64 req, u64 arg); s64 sys_fcntl(s64 fd, u64 cmd, u64 arg);
s64 sys_dup(s64 fd, s64 to); s64 sys_poll(u64 fds, u64 n, s64 ms); s64 sys_statfs(const char *path, u64 buf);
s64 sys_pread_fd(s64 fd, void *buf, u64 n, u64 off);
int fd_is_file(s64 fd);
s64 sys_pipe2(u64 out, u64 flags);
void fd_fork(void); void fd_close_all(void); char *fd_export(void); void fd_import(const char *list);
extern char linux_cwd[1024];   // the working directory (fs.c)

// ---- processes: each its own module instance and memory, the host's (start.c, process.c) ----
HOST("proc_pid") u32 host_proc_pid(void);
HOST("proc_ppid") u32 host_proc_ppid(void);
void linux_run(Cpu *c, u64 pc) __attribute__((noreturn));   // the guest from pc on, entered again at each return address
void linux_resume_at(Cpu *c, u64 pc) __attribute__((noreturn));   // unwinds the runtime's stack, then the guest goes on at pc
s64 sys_fork(Cpu *c, u64 next); s64 sys_execve(const char *path, u64 argv, u64 envp); s64 sys_wait4(s64 pid, u64 status, u64 options);
void linux_exit(u32 code) __attribute__((noreturn));
extern const char *linux_argv0;
