/* =============================================================================
 * intr.c  –  Xtensa LX7 interrupt controller implementation for ESP32-S3
 * ============================================================================= */
#include "intr.h"
#include "xtensa_ops.h"
#include "types.h"

/* ---- Internal ISR dispatch table (one slot per CPU interrupt 0..31) ------ */
static intr_desc_t _isr_table[CPU_INTR_NUM_MAX];

/* ---- CPU interrupt level map (from core-isa.h XCHAL_INTxx_LEVEL) --------- */
/* Maps CPU interrupt slot → Xtensa interrupt level.
 * Only a subset of the 32 CPU interrupts are wirable at each level.
 * This table lists one pre-selected slot per level for general use.
 * Software interrupts 0-3 (CPU intr 14, 16, 17, 28) are reserved by ESP-IDF.
 */
static const uint8_t _level_default_slot[8] = {
    0,   /* level 0 – unused */
    5,   /* level 1 – use CPU intr 5  */
    19,  /* level 2 – use CPU intr 19 */
    23,  /* level 3 – use CPU intr 23 */
    24,  /* level 4 – use CPU intr 24 */
    29,  /* level 5 – use CPU intr 29 */
    0,   /* level 6 – debug, not assignable */
    14,  /* level 7 – NMI, CPU intr 14 */
};

/* Track which CPU interrupt numbers are in use */
static uint32_t _cpu_intr_used_mask = 0;


void intr_init(void)
{
    int i;

    /* Disable all CPU interrupts */
    WSR(INTENABLE, 0);
    RSYNC();

    /* Clear ISR table */
    for (i = 0; i < CPU_INTR_NUM_MAX; i++) {
        _isr_table[i].handler = NULL;
        _isr_table[i].arg     = NULL;
    }
    _cpu_intr_used_mask = 0;
}


int intr_alloc(intr_src_t src, int level, intr_isr_t handler,
               void *arg, int *cpu_intr_out)
{
    int cpu_intr;
    uint32_t old_ps;

    if (level < 1 || level > 7) {
        return -1;
    }

    /* Select the CPU interrupt slot for this level */
    cpu_intr = _level_default_slot[level];

    if (_cpu_intr_used_mask & (1u << cpu_intr)) {
        /* Slot already taken; caller must choose a different level */
        return -1;
    }

    /* Register the handler */
    old_ps = xtensa_intr_disable();
    _isr_table[cpu_intr].handler = handler;
    _isr_table[cpu_intr].arg     = arg;
    _cpu_intr_used_mask |= (1u << cpu_intr);

    /* Map peripheral source → CPU interrupt via interrupt matrix */
    INTMTX_PRO_MAP_REG(src) = (uint32_t)cpu_intr;

    xtensa_intr_restore(old_ps);

    if (cpu_intr_out) {
        *cpu_intr_out = cpu_intr;
    }

    return 0;
}


void intr_enable(int cpu_intr)
{
    uint32_t old_ps = xtensa_intr_disable();
    uint32_t intenable = RSR(INTENABLE);
    intenable |= (1u << cpu_intr);
    WSR(INTENABLE, intenable);
    RSYNC();
    xtensa_intr_restore(old_ps);
}


void intr_disable(int cpu_intr)
{
    uint32_t old_ps = xtensa_intr_disable();
    uint32_t intenable = RSR(INTENABLE);
    intenable &= ~(1u << cpu_intr);
    WSR(INTENABLE, intenable);
    RSYNC();
    xtensa_intr_restore(old_ps);
}


void intr_free(int cpu_intr)
{
    uint32_t old_ps = xtensa_intr_disable();
    /* Disable in INTENABLE */
    uint32_t intenable = RSR(INTENABLE);
    intenable &= ~(1u << cpu_intr);
    WSR(INTENABLE, intenable);
    RSYNC();

    _isr_table[cpu_intr].handler = NULL;
    _isr_table[cpu_intr].arg     = NULL;
    _cpu_intr_used_mask &= ~(1u << cpu_intr);
    xtensa_intr_restore(old_ps);
}


/* ============================================================================
 * Level-2 ISR dispatcher (called from _Level2Vector in vectors.S).
 * Reads INTERRUPT & INTENABLE to find active IRQ, calls registered handler.
 * ============================================================================ */
void IRAM_ATTR _xt_isr_handler_level2(void)
{
    uint32_t active = RSR(INTSET) & RSR(INTENABLE) & 0x00380000u; /* L2 mask */
    int i;
    for (i = 0; i < 32; i++) {
        if ((active >> i) & 1u) {
            if (_isr_table[i].handler) {
                _isr_table[i].handler(_isr_table[i].arg);
            }
            /* Ack edge-triggered interrupts */
            WSR(INTCLEAR, 1u << i);
        }
    }
}


/* Level-3 dispatcher */
void IRAM_ATTR _xt_isr_handler_level3(void)
{
    uint32_t active = RSR(INTSET) & RSR(INTENABLE) & 0x28C08800u; /* L3 mask */
    int i;
    for (i = 0; i < 32; i++) {
        if ((active >> i) & 1u) {
            if (_isr_table[i].handler) {
                _isr_table[i].handler(_isr_table[i].arg);
            }
            WSR(INTCLEAR, 1u << i);
        }
    }
}


/* Level-4,5 – minimal stubs; expand like level2/3 as needed */
void IRAM_ATTR _xt_isr_handler_level4(void) { /* TODO: dispatch L4 IRQs */ }
void IRAM_ATTR _xt_isr_handler_level5(void) { /* TODO: dispatch L5 IRQs */ }
