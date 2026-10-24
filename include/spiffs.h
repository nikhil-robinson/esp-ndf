/* SPIFFS, on-flash compatible with ESP-IDF's spiffs component at its default
 * config (256 B pages, 4 KB blocks, 32 B names, 4 B meta, magic + length).
 * Flat namespace: names are full paths ("/dir/a.txt", < 32 chars).
 * One mounted volume; flags/whence/errors are the fat.h values. */
#pragma once
#include "ndf.h"
#include "fat.h"
#include "partition.h"

int  spiffs_format(const partition_t *p);
int  spiffs_mount(const partition_t *p);       /* -EINVAL (not a SPIFFS volume) -> format */
int  spiffs_unmount(void);

int  spiffs_open(const char *path, int flags);
int  spiffs_read(int fd, void *buf, size_t n);
int  spiffs_write(int fd, const void *buf, size_t n);
int  spiffs_seek(int fd, int32_t off, int whence);
int  spiffs_close(int fd);
int  spiffs_sync(int fd);                      /* data + size durable without closing */
int  spiffs_unlink(const char *path);
int  spiffs_rename(const char *from, const char *to);   /* FAT_EEXIST if to exists */
int  spiffs_stat(const char *path, uint32_t *size, bool *is_dir);
int  spiffs_opendir(const char *path);         /* names under path, '/' kept inside names */
int  spiffs_readdir(int dd, char *name, uint32_t *size, bool *is_dir);
int  spiffs_closedir(int dd);

void spiffs_info(uint32_t *total_bytes, uint32_t *used_bytes);
