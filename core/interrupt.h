/*
 * Interrupt Management
 */

#ifndef CORE_INTERRUPT_H
#define CORE_INTERRUPT_H

#include <stdint.h>

/* Interrupt levels on Xtensa LX7 */
#define INT_LEVEL_1     1   /* Low priority / Exceptions */
#define INT_LEVEL_2     2
#define INT_LEVEL_3     3
#define INT_LEVEL_4     4
#define INT_LEVEL_5     5   /* Debug */
#define INT_LEVEL_NMI   6   /* Non-maskable */

/* Interrupt handler type */
typedef void (*int_handler_t)(void *arg);

/* Disable all interrupts, return previous state */
static inline uint32_t interrupt_disable(void)
{
    uint32_t old_level;
    __asm__ volatile (
        "rsil %0, 15"
        : "=r" (old_level)
    );
    return old_level;
}

/* Enable interrupts to a specific level */
static inline void interrupt_enable(uint32_t level)
{
    __asm__ volatile (
        "wsr %0, PS\n"
        "rsync"
        :: "r" (level)
    );
}

/* Restore interrupt state */
static inline void interrupt_restore(uint32_t state)
{
    __asm__ volatile (
        "wsr %0, PS\n"
        "rsync"
        :: "r" (state)
    );
}

/* Enable specific interrupt number */
void interrupt_enable_num(uint32_t int_num);

/* Disable specific interrupt number */
void interrupt_disable_num(uint32_t int_num);

/* Register interrupt handler */
void interrupt_register(uint32_t int_num, int_handler_t handler, void *arg);

#endif /* CORE_INTERRUPT_H */
