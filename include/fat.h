/* FAT12/16/32 filesystem on a block device, long file names (VFAT).
 * One mounted volume. Paths: "/dir/name.ext" (case-insensitive). */
#pragma once
#include "ndf.h"
#include "bdev.h"

enum { FAT_RDONLY = 0, FAT_WRONLY = 1, FAT_RDWR = 2, FAT_CREAT = 0x40, FAT_TRUNC = 0x200, FAT_APPEND = 0x400 };
enum { FAT_SEEK_SET = 0, FAT_SEEK_CUR = 1, FAT_SEEK_END = 2 };
enum { FAT_OK = 0, FAT_ENOENT = -2, FAT_EIO = -5, FAT_EEXIST = -17, FAT_ENOTDIR = -20,
       FAT_EISDIR = -21, FAT_EINVAL = -22, FAT_EMFILE = -24, FAT_ENOSPC = -28, FAT_ENOTEMPTY = -39 };

int  fat_format(bdev_t *b);                 /* new empty volume */
int  fat_mount(bdev_t *b);                   /* repairs an erased FAT copy from its mirror */
uint32_t fat_repaired(void);                /* FAT sectors repaired by the last mount */
int  fat_unmount(void);
int  fat_sync(void);

int  fat_open(const char *path, int flags);
int  fat_read(int fd, void *buf, size_t n);
int  fat_write(int fd, const void *buf, size_t n);
int  fat_seek(int fd, int32_t off, int whence);    /* returns new position */
int  fat_close(int fd);

int  fat_unlink(const char *path);          /* file or empty directory */
int  fat_mkdir(const char *path);
int  fat_stat(const char *path, uint32_t *size, bool *is_dir);

int  fat_opendir(const char *path);
/* next entry: name (<= 255 + NUL), returns 1, 0 at end, < 0 error */
int  fat_readdir(int dd, char *name, uint32_t *size, bool *is_dir);
int  fat_closedir(int dd);

void fat_info(uint32_t *total_bytes, uint32_t *free_bytes, int *fat_bits);
