/* VFS: one path namespace over several filesystems. A filesystem registers
 * under a prefix ("/fat"); vfs_open("/fat/a.txt") goes to its open("/a.txt").
 * fd 0..2 are the console (read 0, write 1/2). */
#pragma once
#include "ndf.h"

typedef struct {
    int (*open)(const char *path, int flags);
    int (*read)(int fd, void *buf, size_t n);
    int (*write)(int fd, const void *buf, size_t n);
    int (*seek)(int fd, int32_t off, int whence);
    int (*close)(int fd);
    int (*unlink)(const char *path);
    int (*mkdir)(const char *path);
    int (*stat)(const char *path, uint32_t *size, bool *is_dir);
    int (*opendir)(const char *path);
    int (*readdir)(int dd, char *name, uint32_t *size, bool *is_dir);
    int (*closedir)(int dd);
} vfs_ops_t;          /* any op may be NULL -> -22 (EINVAL) */

extern const vfs_ops_t vfs_fat_ops;   /* FAT (fat.h) as a VFS filesystem */

int vfs_register(const char *prefix, const vfs_ops_t *ops);
int vfs_unregister(const char *prefix);

/* flags / whence / errors: same values as fat.h (FAT_RDONLY..., FAT_ENOENT...) */
int vfs_open(const char *path, int flags);
int vfs_read(int fd, void *buf, size_t n);
int vfs_write(int fd, const void *buf, size_t n);
int vfs_seek(int fd, int32_t off, int whence);
int vfs_close(int fd);
int vfs_unlink(const char *path);
int vfs_mkdir(const char *path);
int vfs_stat(const char *path, uint32_t *size, bool *is_dir);
int vfs_opendir(const char *path);
int vfs_readdir(int dd, char *name, uint32_t *size, bool *is_dir);
int vfs_closedir(int dd);
