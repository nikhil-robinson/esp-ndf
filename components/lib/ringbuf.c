/* Offsets run 0..size. Empty: head == tail. The producer never lets head
 * catch up with tail from behind (keeps >= 4 bytes gap), and wraps either at
 * size or by writing MARK where the next item would not fit. */
#include "ringbuf.h"
#include <string.h>

#define MARK 0xFFFFFFFFu
#define HDR  4u
static inline void barrier(void) { __asm__ volatile("memw" ::: "memory"); }
static inline uint32_t need_of(uint32_t len) { return HDR + ((len + 3) & ~3u); }

void rb_init(ringbuf_t *rb, void *buf, uint32_t size)
{
    uintptr_t a = ((uintptr_t)buf + 3) & ~(uintptr_t)3;            /* records are word headers: align the start */
    size -= size < a - (uintptr_t)buf ? size : a - (uintptr_t)buf;
    rb->buf = (uint8_t *)a; rb->size = size & ~3u; rb->head = rb->tail = 0; rb->reserved = 0;
}

void *rb_reserve(ringbuf_t *rb, uint32_t len)
{
    uint32_t need = need_of(len), h = rb->head, t = rb->tail;
    if (len >= MARK - HDR) return 0;
    if (h >= t) {
        if (need <= rb->size - h) goto at;                 /* fits before the end */
        if (need < t) {                                     /* wrap: mark the end, write at 0 */
            if (rb->size - h >= HDR) *(volatile uint32_t *)(rb->buf + h) = MARK;
            h = 0; goto at;
        }
        return 0;
    }
    if (need < t - h) goto at;                            /* behind the reader, keep a gap */
    return 0;
at:
    *(uint32_t *)(rb->buf + h) = len;
    rb->reserved = h + need;                                /* new head after commit */
    return rb->buf + h + HDR;
}

void rb_commit(ringbuf_t *rb) { barrier(); rb->head = rb->reserved; }

bool rb_put(ringbuf_t *rb, const void *data, uint32_t len)
{
    void *p = rb_reserve(rb, len);
    if (!p) return false;
    memcpy(p, data, len);
    rb_commit(rb);
    return true;
}

void *rb_peek(ringbuf_t *rb, uint32_t *len)
{
    uint32_t t = rb->tail, h = rb->head;
    if (t == h) return 0;
    barrier();
    if (t == rb->size || *(uint32_t *)(rb->buf + t) == MARK) {   /* producer wrapped */
        t = 0; rb->tail = 0;
        if (t == h) return 0;
    }
    *len = *(uint32_t *)(rb->buf + t);
    return rb->buf + t + HDR;
}

void rb_release(ringbuf_t *rb)
{
    uint32_t t = rb->tail, len = *(uint32_t *)(rb->buf + t);
    barrier();
    rb->tail = t + need_of(len);
}

int rb_get(ringbuf_t *rb, void *out, uint32_t max)
{
    uint32_t len;
    void *p = rb_peek(rb, &len);
    if (!p) return -1;
    if (len > max) return -2;
    memcpy(out, p, len);
    rb_release(rb);
    return (int)len;
}

uint32_t rb_used(const ringbuf_t *rb)
{
    uint32_t h = rb->head, t = rb->tail;
    return h >= t ? h - t : rb->size - t + h;
}
