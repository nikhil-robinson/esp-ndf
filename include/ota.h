/* Over-the-air update, ESP-IDF on-flash format (otadata select entries, ota_N app partitions): IDF's tools and IDF
 * apps read and write the same state. An OTA image is build/<app>/ota.bin (RAM image, its code window at
 * +NDF_OTA_XIP_OFF). With rollback (NDF_OTA_ROLLBACK) a new image must call ota_mark_valid() during its first boot,
 * else the loader drops it at the next reset and boots the previous one. */
#pragma once
#include "ndf.h"
#include "partition.h"

typedef struct { const partition_t *part; uint32_t written; bool erased_all; } ota_t;
enum { OTA_NEW = 0, OTA_PENDING_VERIFY = 1, OTA_VALID = 2, OTA_INVALID = 3, OTA_ABORTED = 4, OTA_UNDEFINED = -1 };

const partition_t *ota_running(void);           /* the slot (or factory) this image was loaded from */
const partition_t *ota_next(void);              /* the slot an update should go to */
int  ota_begin(ota_t *o, const partition_t *slot);   /* erases the slot (whole: ota_end checks every byte) */
int  ota_write(ota_t *o, const void *data, size_t len);   /* sequential */
int  ota_end(ota_t *o);                         /* signature + image header + SHA-256 checked: 0 = good */
/* Authenticity: images carry a signed trailer (tools/ota_sign.py: ECDSA P-256 over the whole image + version).
 * With NDF_OTA_REQUIRE_SIG (default) ota_end refuses everything until a key is set; versions below min_version
 * (rollback) are refused. ota_set_boot only takes a slot ota_end accepted. */
int  ota_set_key(const uint8_t pub[64], uint32_t min_version);
int  ota_set_boot(const partition_t *slot);     /* next reset boots it (state NEW) */
int  ota_mark_valid(void);                      /* confirm the running image (rollback off for it) */
int  ota_state(void);                           /* state of the running image's select entry, OTA_UNDEFINED if none */
