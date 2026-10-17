/* Non-volatile storage, on-flash format compatible with ESP-IDF NVS (v2:
 * page version 0xFE, multi-page blobs). A partition written by either side
 * can be read by the other. Writes are immediate (commit is a no-op). */
#pragma once
#include "ndf.h"

typedef enum { NVS_OK = 0, NVS_NOT_FOUND = -1, NVS_NO_SPACE = -2, NVS_ERR = -3, NVS_TOO_BIG = -4 } nvs_err_t;
typedef uint8_t nvs_handle_t;                 /* namespace index */

int  nvs_mount(const char *partition_label);  /* scan + index; formats blank pages */
int  nvs_open(const char *ns, nvs_handle_t *h);   /* creates the namespace on first use */

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

int  nvs_erase_key(nvs_handle_t, const char *k);
int  nvs_erase_all(void);                     /* erase the whole partition */
void nvs_stats(int *used_entries, int *free_entries);
