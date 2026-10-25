/* Event dispatch without an RTOS: post from anywhere (ISR, either core), handlers run in the loop that calls
 * event_dispatch(). Fixed queue (NDF_EVENT_QUEUE, power of 2), drops are counted, never silent. */
#pragma once
#include "ndf.h"

#ifndef NDF_EVENT_QUEUE
#define NDF_EVENT_QUEUE 64
#endif
#define EVENT_ANY 0xFFFF

typedef void (*event_fn_t)(uint16_t id, uint32_t data, void *arg);
int      event_on(uint16_t id, event_fn_t fn, void *arg);   /* up to 16 handlers; EVENT_ANY gets every id */
void     event_off(event_fn_t fn);
int      event_post(uint16_t id, uint32_t data);           /* 0, or -1 queue full (counted) */
int      event_dispatch(void);                              /* runs the handlers for all queued events */
uint32_t event_dropped(void);
