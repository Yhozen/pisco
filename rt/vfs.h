// SPDX-License-Identifier: GPL-2.0-or-later
// The app's file system (vfs.c). Paths are guest paths (UTF-8); errors are Win32 error codes.
#pragma once
typedef struct VFile VFile;
VFile *vfs_open(const char *path, u32 access, u32 disposition, u32 *err); // *err may be 183 on success
void vfs_close(VFile *f);
int vfs_is_dir(VFile *f);
s64 vfs_read(VFile *f, void *dst, u64 n);
s64 vfs_write(VFile *f, const void *src, u64 n);
u64 vfs_size(VFile *f);
u64 *vfs_pos(VFile *f);
void vfs_truncate(VFile *f, u64 size);
u64 vfs_mtime(VFile *f);
const char *vfs_file_path(VFile *f);
u32 vfs_stat(const char *path, u64 *size);   // 0 nothing, 1 file, 2 directory
u32 vfs_mkdir(const char *path);
u32 vfs_remove(const char *path, int dir);
u32 vfs_rename(const char *from, const char *to, int replace);
void vfs_list(const char *path, void (*cb)(void *ctx, const char *name, int dir, u64 size), void *ctx);
int vfs_path(const char *in, char *out, size_t cap);
