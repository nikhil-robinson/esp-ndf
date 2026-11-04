/* Event queue: one ring, a spinlock (S32C1I) + interrupts off around the index update, so ISRs and both
 * cores may post; dispatch copies an entry out before calling handlers (they may post again). */
#include "event.h"
#include "xcore.h"
#include "cpu.h"

typedef struct { uint16_t id; uint32_t data; } ev_t;
static ev_t q[NDF_EVENT_QUEUE];
static volatile uint32_t head, tail, dropped;
static xc_spin_t lock;
static struct { uint16_t id; event_fn_t fn; void *arg; } h[16];

int event_on(uint16_t id, event_fn_t fn, void *arg)
{
    for (int i = 0; i < 16; i++) if (!h[i].fn) { h[i].id = id; h[i].arg = arg; h[i].fn = fn; return 0; }
    return -1;
}

void event_off(event_fn_t fn) { for (int i = 0; i < 16; i++) if (h[i].fn == fn) h[i].fn = 0; }

RAM_FN int event_post(uint16_t id, uint32_t data)
{
    uint32_t ps = irq_lock();
    xc_spin_lock(&lock);
    int r = 0;
    if (head - tail < NDF_EVENT_QUEUE) { q[head % NDF_EVENT_QUEUE] = (ev_t){ id, data }; xc_barrier(); head++; }
    else { dropped++; r = -1; }
    xc_spin_unlock(&lock);
    irq_unlock(ps);
    return r;
}

int event_dispatch(void)
{
    int n = 0;
    while (tail != head) {
        ev_t e = q[tail % NDF_EVENT_QUEUE];
        xc_barrier();
        tail++;                                     /* single consumer: no lock needed */
        for (int i = 0; i < 16; i++) if (h[i].fn && (h[i].id == e.id || h[i].id == EVENT_ANY)) h[i].fn(e.id, e.data, h[i].arg);
        n++;
    }
    return n;
}

uint32_t event_dropped(void) { return dropped; }
