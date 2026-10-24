/* Cooperative tasks (no RTOS, no preemption): each task has its own stack and gives the CPU back with
 * coop_yield() / coop_sleep_ms(). A switch saves only the CALL0 callee-saved registers (a0, a1, a12-a15),
 * so it costs tens of cycles. Interrupts keep working; ISRs talk to tasks through flags/queues. */
#pragma once
#include "ndf.h"

int  coop_spawn(void (*fn)(void *), void *arg, void *stack, size_t stack_bytes);   /* task id, -1 full */
void coop_yield(void);
void coop_sleep_ms(uint32_t ms);
void coop_run(void);           /* the caller becomes the scheduler; returns when every task has returned */
int  coop_self(void);
