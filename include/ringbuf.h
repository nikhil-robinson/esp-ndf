/* Byte ring buffer for variable-length items (messages, packets, log lines).
 * One producer, one consumer: ISR <-> main or core <-> core, lock-free.
 * Each item is stored contiguously (4-byte length header + data padded to 4),
 * so both sides can work in place: reserve/commit to write, peek/release to
 * read, no copy. rb_put / rb_get are the copying shortcuts.
 * buf must be 4-byte aligned, size a multiple of 4; the largest item that
 * always fits is size/2 - 8. */
#pragma once
#include "ndf.h"

typedef struct {
    uint8_t *buf;
    uint32_t size;
    volatile uint32_t head;      /* producer: next write offset */
    uint32_t _pad[5];            /* head and tail on separate 32-byte lines */
    volatile uint32_t tail;      /* consumer: next read offset */
    uint32_t reserved;           /* producer: bytes of the pending reservation */
} ringbuf_t;

void  rb_init(ringbuf_t *rb, void *buf, uint32_t size);
void *rb_reserve(ringbuf_t *rb, uint32_t len);          /* NULL if no room */
void  rb_commit(ringbuf_t *rb);                         /* publish the reserved item */
bool  rb_put(ringbuf_t *rb, const void *data, uint32_t len);
void *rb_peek(ringbuf_t *rb, uint32_t *len);            /* oldest item in place, NULL if empty */
void  rb_release(ringbuf_t *rb);                        /* drop the peeked item */
int   rb_get(ringbuf_t *rb, void *out, uint32_t max);   /* copy out; -1 empty, -2 item larger than max (left in place) */
uint32_t rb_used(const ringbuf_t *rb);                  /* bytes in use (headers included) */
