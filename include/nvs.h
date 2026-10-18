/* Non-volatile storage, on-flash format compatible with ESP-IDF NVS (v2:
 * page version 0xFE, multi-page blobs). A partition written by either side
 * can be read by the other. Writes are immediate (commit is a no-op). */
#pragma once
#include "ndf.h"

typedef enum { NVS_OK = 0, NVS_NOT_FOUND = -1, NVS_NO_SPACE = -2, NVS_ERR = -3, NVS_TOO_BIG = -4 } nvs_err_t;
typedef uint8_t nvs_handle_t;                 /* namespace index */

int  nvs_mount(const char *partition_label);  /* scan + index; formats blank pages */
int  nvs_open(const char *ns, nvs_handle_t *h);
/* Encrypted NVS (IDF NVS encryption): XTS-AES-256, key = eky[32] | tky[32]. */
int  nvs_mount_secure(const char *partition_label, const uint8_t key[64]);
/* keys from an nvs_keys partition (as written by tools/nvstool.py keygen) */
int  nvs_read_keys(const char *keys_partition_label, uint8_t key[64]);   /* creates the namespace on first use */

int  nvs_set_u8(nvs_handle_t, const char *k, uint8_t v);
int  nvs_set_i8(nvs_handle_t, const char *k, int8_t v);
int  nvs_set_u16(nvs_handle_t, const char *k, uint16_t v);
int  nvs_set_i16(nvs_handle_t, const char *k, int16_t v);
int  nvs_set_u32(nvs_handle_t, const char *k, uint32_t v);
int  nvs_set_i32(nvs_handle_t, const char *k, int32_t v);
int  nvs_set_u64(nvs_handle_t, const char *k, uint64_t v);
int  nvs_set_i64(nvs_handle_t, const char *k, int64_t v);
int  nvs_set_str(nvs_handle_t, const char *k, const char *s);
int  nvs_set_blob(nvs_handle_t, const char *k, const void *b, size_t n);

int  nvs_get_u8(nvs_handle_t, const char *k, uint8_t *v);
int  nvs_get_i8(nvs_handle_t, const char *k, int8_t *v);
int  nvs_get_u16(nvs_handle_t, const char *k, uint16_t *v);
int  nvs_get_i16(nvs_handle_t, const char *k, int16_t *v);
int  nvs_get_u32(nvs_handle_t, const char *k, uint32_t *v);
int  nvs_get_i32(nvs_handle_t, const char *k, int32_t *v);
int  nvs_get_u64(nvs_handle_t, const char *k, uint64_t *v);
int  nvs_get_i64(nvs_handle_t, const char *k, int64_t *v);
/* *len in: buffer size, out: bytes needed/written (string includes the NUL) */
int  nvs_get_str(nvs_handle_t, const char *k, char *s, size_t *len);
int  nvs_get_blob(nvs_handle_t, const char *k, void *b, size_t *len);

/* Boot-time read (like IDF's nvs_bootloader_read): no mount, no RAM index,
 * stack only. Fill ns/key/type (+ str/len for strings); results per item. */
enum { NVS_TYPE_U8 = 0x01, NVS_TYPE_I8 = 0x11, NVS_TYPE_U16 = 0x02, NVS_TYPE_I16 = 0x12, NVS_TYPE_U32 = 0x04,
       NVS_TYPE_I32 = 0x14, NVS_TYPE_U64 = 0x08, NVS_TYPE_I64 = 0x18, NVS_TYPE_STR = 0x21 };
typedef struct {
    const char *ns, *key;
    uint8_t type;                   /* NVS_TYPE_* */
    int result;                     /* NVS_OK / NVS_NOT_FOUND / NVS_ERR (type mismatch, crc) / NVS_TOO_BIG */
    uint8_t ns_index;
    char *str; size_t len;          /* strings: buffer and size in, length (incl. NUL) out */
    union { uint8_t u8; int8_t i8; uint16_t u16; int16_t i16; uint32_t u32; int32_t i32; uint64_t u64; int64_t i64; };
} nvs_boot_item_t;
int  nvs_boot_read(const char *partition_label, nvs_boot_item_t *items, int n);

int  nvs_erase_key(nvs_handle_t, const char *k);
int  nvs_erase_all(void);                     /* erase the whole partition */
void nvs_stats(int *used_entries, int *free_entries);
