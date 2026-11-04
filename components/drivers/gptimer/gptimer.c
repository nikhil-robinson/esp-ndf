/* GPTimer on TIMG. Timer t of group g: registers at TIMG_BASE(g) + 0x24 * t. */
#include "gptimer.h"
#include "spin.h"
#include "clk.h"
#include "soc.h"

#define T_BASE(id)   (TIMG_BASE((id) >> 1) + 0x24 * ((id) & 1))
#define T_CONFIG(id) (T_BASE(id) + 0x00)   /* b31 EN b30 INCREASE b29 AUTORELOAD b28:13 DIV b10 ALARM_EN b9 USE_XTAL */
#define T_LO(id)     (T_BASE(id) + 0x04)
#define T_HI(id)     (T_BASE(id) + 0x08)
#define T_UPDATE(id) (T_BASE(id) + 0x0C)   /* b31: latch counter into LO/HI */
#define T_ALARMLO(id)(T_BASE(id) + 0x10)
#define T_ALARMHI(id)(T_BASE(id) + 0x14)
#define T_LOADLO(id) (T_BASE(id) + 0x18)
#define T_LOADHI(id) (T_BASE(id) + 0x1C)
#define T_LOAD(id)   (T_BASE(id) + 0x20)
#define G_INT_ENA(id)(TIMG_BASE((id) >> 1) + 0x70)
#define G_INT_CLR(id)(TIMG_BASE((id) >> 1) + 0x7C)
#define G_INT_ST(id) (TIMG_BASE((id) >> 1) + 0x78)
#define G_REGCLK(id) (TIMG_BASE((id) >> 1) + 0xFC)

#define CFG_EN       BIT(31)
#define CFG_INC      BIT(30)
#define CFG_AUTOREL  BIT(29)
#define CFG_ALARM    BIT(10)
#define CFG_XTAL     BIT(9)

#ifndef GPT_SRCS                                   /* chip hooks (drivers/gptimer/esp32p4/gptimer.c sets its own) */
#define GPT_SRCS { 50, 51, 53, 54 }
#define GPT_APB_HZ 80000000u                        /* 0: no APB source */
#define GPT_CLK_SEL(id, xtal) ((xtal) ? CFG_XTAL : 0)   /* config bits for the source */
#endif
static const uint8_t src[GPT_COUNT] = GPT_SRCS;

typedef struct { irq_fn_t fn; void *arg; int cpu_int; bool periodic; } gpt_t;
static gpt_t gpt[GPT_COUNT];

uint32_t gpt_init(int id, uint32_t hz)
{
    if (!(id >= 0 && id < GPT_COUNT)) return 0;      /* caller index: never past the hardware */
    if (id < 0 || id >= GPT_COUNT || !hz) return 0;
    uint32_t base, div;
    bool xtal;
    if (GPT_APB_HZ && GPT_APB_HZ % hz == 0 && (div = GPT_APB_HZ / hz) >= 2 && div <= 65536) xtal = false;
    else if (40000000 % hz == 0 && (div = 40000000 / hz) >= 2 && div <= 65536) xtal = true;
    else return 0;                                  /* no exact divider: refuse, don't round */
    periph_enable((id >> 1) ? PERIPH_TIMG1 : PERIPH_TIMG0);
    REG_SET(G_REGCLK(id), BIT(31));
    REG(T_CONFIG(id)) = 0;
    base = GPT_CLK_SEL(id, xtal);
    REG(T_CONFIG(id)) = base | CFG_INC | (div & 0xFFFF) << 13;  /* 65536 encodes as 0 */
    gpt_set(id, 0);
    return (xtal ? 40000000 : GPT_APB_HZ) / div;
}

void gpt_start(int id) { REG_SET(T_CONFIG(id), CFG_EN); }
void gpt_stop(int id)  { REG_CLR(T_CONFIG(id), CFG_EN); }

void gpt_set(int id, uint64_t c)
{
    if (!(id >= 0 && id < GPT_COUNT)) return;      /* caller index: never past the hardware */
    REG(T_LOADLO(id)) = (uint32_t)c;
    REG(T_LOADHI(id)) = (uint32_t)(c >> 32);
    REG(T_LOAD(id)) = 1;
}

uint64_t gpt_get(int id)
{
    REG(T_UPDATE(id)) = BIT(31);
    SPIN_US(!(REG(T_UPDATE(id)) & BIT(31)), 100);   /* latched when bit self-clears */
    return (uint64_t)REG(T_HI(id)) << 32 | REG(T_LO(id));
}

uint32_t gpt_get32(int id)                          /* low word only: one register read less */
{
    REG(T_UPDATE(id)) = BIT(31);
    SPIN_US(!(REG(T_UPDATE(id)) & BIT(31)), 100);
    return REG(T_LO(id));
}

RAM_FN static void gpt_isr(void *arg)
{
    int id = (int)arg;
    if (!(REG(G_INT_ST(id)) & BIT(id & 1))) return;           /* shared CPU line: not ours */
    REG(G_INT_CLR(id)) = BIT(id & 1);
    if (gpt[id].periodic) REG_SET(T_CONFIG(id), CFG_ALARM);   /* hw clears ALARM_EN on fire */
    gpt[id].fn(gpt[id].arg);
}

int gpt_alarm(int id, uint64_t alarm, bool periodic, irq_fn_t fn, void *arg, int level)
{
    if (!(id >= 0 && id < GPT_COUNT)) return -1;      /* caller index: never past the hardware */
    if (id < 0 || id >= GPT_COUNT) return -1;
    gpt_alarm_off(id);
    REG(T_ALARMLO(id)) = (uint32_t)alarm;
    REG(T_ALARMHI(id)) = (uint32_t)(alarm >> 32);
    REG_FIELD(T_CONFIG(id), 29, 1, periodic);       /* autoreload to LOAD (0) */
    if (periodic) { REG(T_LOADLO(id)) = 0; REG(T_LOADHI(id)) = 0; }
    gpt[id] = (gpt_t){ fn, arg, -1, periodic };
    if (fn) {
        gpt[id].cpu_int = level == 7 ? irq_attach_nmi(src[id], gpt_isr, (void *)id) : irq_attach(src[id], level, gpt_isr, (void *)id);   /* 7 = NMI */
        if (gpt[id].cpu_int < 0) return -1;
        REG(G_INT_CLR(id)) = BIT(id & 1);
        REG_SET(G_INT_ENA(id), BIT(id & 1));
    }
    REG_SET(T_CONFIG(id), CFG_ALARM);
    return 0;
}

void gpt_alarm_off(int id)
{
    REG_CLR(T_CONFIG(id), CFG_ALARM);
    REG_CLR(G_INT_ENA(id), BIT(id & 1));
    if (gpt[id].cpu_int >= 0 && gpt[id].fn) irq_detach(src[id], gpt[id].cpu_int);
    gpt[id].fn = 0;
    gpt[id].cpu_int = -1;
}
