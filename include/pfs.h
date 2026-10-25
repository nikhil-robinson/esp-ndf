/* PFS: power-safe flash filesystem (own format, LittleFS-class guarantees), for logs and settings that must
 * survive a power cut at any instant. Same API shape as spiffs.h (flat names "/dir/a.txt", fat.h flags/errors).
 *
 * Guarantee: every flush (buffer full, large write, sync, close) and every create / truncate / unlink / rename is
 * atomic: after a cut the volume shows either the old or the new state, never a torn one, and mount never needs a
 * repair pass. Bytes written but not yet flushed (at most one 256 B buffer per open file) are lost on a cut.
 * How: data is only ever written to erased space (copy-on-write); the metadata is a CRC-32 record log in two
 * ping-pong halves, compacted into the other half when full (the new half counts only once its COMMIT record is
 * written). Unflushed or torn records fail their CRC and are ignored. */
#pragma once
#include "ndf.h"
#include "fat.h"
#include "partition.h"

int  pfs_format(const partition_t *p);
int  pfs_mount(const partition_t *p);           /* -EINVAL (no PFS volume) -> format */
int  pfs_unmount(void);

int  pfs_open(const char *path, int flags);
int  pfs_read(int fd, void *buf, size_t n);
int  pfs_write(int fd, const void *buf, size_t n);
int  pfs_seek(int fd, int32_t off, int whence);
int  pfs_sync(int fd);                          /* flush: durable from here on */
int  pfs_close(int fd);
int  pfs_unlink(const char *path);
int  pfs_rename(const char *from, const char *to);   /* atomic; FAT_EEXIST if to exists */
int  pfs_stat(const char *path, uint32_t *size, bool *is_dir);
int  pfs_opendir(const char *path);
int  pfs_readdir(int dd, char *name, size_t name_max, uint32_t *size, bool *is_dir);
int  pfs_closedir(int dd);
void pfs_info(uint32_t *total_bytes, uint32_t *used_bytes);

/* test hook: after n more flash program/erase operations, the next one is torn (half the bytes programmed, or the
 * erase not done) and every later one fails, as if the power was cut. -1 = off. Remount to "power up" again. */
void pfs_fault_after(int n);
