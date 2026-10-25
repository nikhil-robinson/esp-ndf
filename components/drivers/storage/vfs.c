/* VFS: prefix mount table + global fd table mapping to per-filesystem fds. */
#include "vfs.h"
#include "ndf_config.h"
#include "fat.h"
#include "spiffs.h"
#include "pfs.h"
#include "console.h"
#include <string.h>

#define MAX_FS NDF_VFS_MAX_MOUNTS
#define MAX_FD NDF_VFS_MAX_FILES           /* files and directories share the table */
#define FD0    3            /* 0..2 = console */

static struct { char prefix[16]; size_t len; const vfs_ops_t *ops; } fs[MAX_FS];
static struct { const vfs_ops_t *ops; int16_t fd; bool used, dir; } fds[MAX_FD];

const vfs_ops_t vfs_fat_ops = { fat_open, fat_read, fat_write, fat_seek, fat_close, fat_unlink,
                                fat_mkdir, fat_stat, fat_opendir, fat_readdir, fat_closedir };
const vfs_ops_t vfs_spiffs_ops = { spiffs_open, spiffs_read, spiffs_write, spiffs_seek, spiffs_close, spiffs_unlink,
                                   NULL, spiffs_stat, spiffs_opendir, spiffs_readdir, spiffs_closedir };
const vfs_ops_t vfs_pfs_ops = { pfs_open, pfs_read, pfs_write, pfs_seek, pfs_close, pfs_unlink,
                                NULL, pfs_stat, pfs_opendir, pfs_readdir, pfs_closedir };

int vfs_register(const char *prefix, const vfs_ops_t *ops)
{
    size_t n = strlen(prefix);
    if (n == 0 || n >= sizeof fs[0].prefix || prefix[0] != '/' || prefix[n - 1] == '/') return FAT_EINVAL;
    int free_i = -1;
    for (int i = 0; i < MAX_FS; i++) {
        if (fs[i].ops && !strcmp(fs[i].prefix, prefix)) return FAT_EEXIST;
        if (!fs[i].ops && free_i < 0) free_i = i;
    }
    if (free_i < 0) return FAT_EMFILE;
    strcpy(fs[free_i].prefix, prefix);
    fs[free_i].len = n;
    fs[free_i].ops = ops;
    return 0;
}

int vfs_unregister(const char *prefix)
{
    for (int i = 0; i < MAX_FS; i++)
        if (fs[i].ops && !strcmp(fs[i].prefix, prefix)) {
            for (int f = 0; f < MAX_FD; f++)
                if (fds[f].used && fds[f].ops == fs[i].ops) return FAT_EINVAL;  /* still open */
            fs[i].ops = NULL;
            return 0;
        }
    return FAT_ENOENT;
}

/* longest matching prefix; *rest = path inside the filesystem ("/" if none) */
static const vfs_ops_t *route(const char *path, const char **rest)
{
    int best = -1;
    for (int i = 0; i < MAX_FS; i++)
        if (fs[i].ops && !strncmp(path, fs[i].prefix, fs[i].len) &&
            (path[fs[i].len] == '/' || path[fs[i].len] == 0) &&
            (best < 0 || fs[i].len > fs[best].len)) best = i;
    if (best < 0) return NULL;
    *rest = path[fs[best].len] ? path + fs[best].len : "/";
    return fs[best].ops;
}

static int fd_new(const vfs_ops_t *ops, int inner, bool dir)
{
    for (int i = 0; i < MAX_FD; i++)
        if (!fds[i].used) { fds[i].ops = ops; fds[i].fd = inner; fds[i].used = true; fds[i].dir = dir; return i + FD0; }
    return FAT_EMFILE;
}

static int fd_get(int fd, bool dir)
{
    int i = fd - FD0;
    return i >= 0 && i < MAX_FD && fds[i].used && fds[i].dir == dir ? i : -1;
}

#define ROUTE(op, path, ...) do { \
        const char *r; const vfs_ops_t *o = route(path, &r); \
        if (!o) return FAT_ENOENT; \
        if (!o->op) return FAT_EINVAL; \
        return o->op(r, ##__VA_ARGS__); \
    } while (0)

int vfs_unlink(const char *path) { ROUTE(unlink, path); }
int vfs_mkdir(const char *path)  { ROUTE(mkdir, path); }
int vfs_stat(const char *path, uint32_t *size, bool *is_dir) { ROUTE(stat, path, size, is_dir); }

static int open_(const char *path, int flags, bool dir)
{
    const char *r;
    const vfs_ops_t *o = route(path, &r);
    if (!o) return FAT_ENOENT;
    if (dir ? !o->opendir : !o->open) return FAT_EINVAL;
    int inner = dir ? o->opendir(r) : o->open(r, flags);
    if (inner < 0) return inner;
    int fd = fd_new(o, inner, dir);
    if (fd < 0) { if (dir) o->closedir(inner); else o->close(inner); }
    return fd;
}

int vfs_open(const char *path, int flags) { return open_(path, flags, false); }
int vfs_opendir(const char *path)         { return open_(path, 0, true); }

int vfs_read(int fd, void *buf, size_t n)
{
    int i = fd_get(fd, false);
    if (i < 0) return FAT_EINVAL;
    return fds[i].ops->read ? fds[i].ops->read(fds[i].fd, buf, n) : FAT_EINVAL;
}

int vfs_write(int fd, const void *buf, size_t n)
{
    if (fd == 1 || fd == 2) return console_write(buf, n);
    int i = fd_get(fd, false);
    if (i < 0) return FAT_EINVAL;
    return fds[i].ops->write ? fds[i].ops->write(fds[i].fd, buf, n) : FAT_EINVAL;
}

int vfs_seek(int fd, int32_t off, int whence)
{
    int i = fd_get(fd, false);
    if (i < 0) return FAT_EINVAL;
    return fds[i].ops->seek ? fds[i].ops->seek(fds[i].fd, off, whence) : FAT_EINVAL;
}

int vfs_readdir(int dd, char *name, size_t name_max, uint32_t *size, bool *is_dir)
{
    int i = fd_get(dd, true);
    if (i < 0) return FAT_EINVAL;
    return fds[i].ops->readdir(fds[i].fd, name, name_max, size, is_dir);
}

static int close_(int fd, bool dir)
{
    int i = fd_get(fd, dir);
    if (i < 0) return FAT_EINVAL;
    int r = dir ? fds[i].ops->closedir(fds[i].fd) : fds[i].ops->close(fds[i].fd);
    fds[i].used = false;
    return r;
}

int vfs_close(int fd)    { return close_(fd, false); }
int vfs_closedir(int dd) { return close_(dd, true); }
