/*
 * Interrupt Management Implementation
 */

#include "interrupt.h"
#include "../arch/esp32s3/esp32s3.h"

/* Interrupt handler table */
#define MAX_INT_HANDLERS 32

static struct {
    int_handler_t handler;
    void *arg;
} int_handlers[MAX_INT_HANDLERS];

void interrupt_enable_num(uint32_t int_num)
{
    if (int_num >= MAX_INT_HANDLERS) return;
    
    uint32_t mask;
    __asm__ volatile ("rsr %0, intenable" : "=r" (mask));
    mask |= (1 << int_num);
    __asm__ volatile ("wsr %0, intenable" :: "r" (mask));
}

void interrupt_disable_num(uint32_t int_num)
{
    if (int_num >= MAX_INT_HANDLERS) return;
    
    uint32_t mask;
    __asm__ volatile ("rsr %0, intenable" : "=r" (mask));
    mask &= ~(1 << int_num);
    __asm__ volatile ("wsr %0, intenable" :: "r" (mask));
}

void interrupt_register(uint32_t int_num, int_handler_t handler, void *arg)
{
    if (int_num >= MAX_INT_HANDLERS) return;
    
    uint32_t state = interrupt_disable();
    
    int_handlers[int_num].handler = handler;
    int_handlers[int_num].arg = arg;
    
    interrupt_restore(state);
}

/* Called from assembly vectors */
void interrupt_dispatch(uint32_t int_num)
{
    if (int_num < MAX_INT_HANDLERS && int_handlers[int_num].handler) {
        int_handlers[int_num].handler(int_handlers[int_num].arg);
    }
}
