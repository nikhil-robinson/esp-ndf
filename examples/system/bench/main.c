/* Benchmark on esp-ndf: 20 kHz loop on both cores, 10 kHz TIMG0 ISR on
 * core 0, 1 kHz USB telemetry. Same bench.h as the IDF and esp-hal builds. */
#include "soc.h"
#include "sys.h"
#include "irq.h"
#include "console.h"
#include "bench.h"

#define ISR_LEVEL 1
#define T0_RUN (BIT(31) | BIT(30) | BIT(29) | 8u << 13)   /* en, up, autoreload, APB/8 = 10 MHz */

static int bench_usb_tx(const char *buf, int n) { return console_try_write(buf, n); }

RAM_FN static void timer_isr(void *arg)
{
    (void)arg;
    REG(TIMG_INT_CLR(0)) = 1;
    REG(TIMG_T0CONFIG(0)) = T0_RUN | BIT(10);              /* re-arm alarm */
    bench_isr();
}

static void timer_start(void)
{
    REG_SET(SYSTEM_PERIP_CLK0, PERIP0_TIMG0); REG_CLR(SYSTEM_PERIP_RST0, PERIP0_TIMG0);
    REG(TIMG_T0CONFIG(0)) = 0;
    REG(TIMG_T0LOADLO(0)) = 0; REG(TIMG_T0LOADHI(0)) = 0; REG(TIMG_T0LOAD(0)) = 1;
    REG(TIMG_T0ALARMLO(0)) = 10000000 / ISR_HZ; REG(TIMG_T0ALARMHI(0)) = 0;
    REG(TIMG_INT_ENA(0)) = 1;
    int n = irq_attach(SRC_TG0_T0, ISR_LEVEL, timer_isr, 0);
    printf("timer isr on cpu int %d (level %d)\n", n, ISR_LEVEL);
    REG(TIMG_T0CONFIG(0)) = T0_RUN | BIT(10);
    __asm__ volatile("rsil a2, 0" ::: "a2");
}

static void gpio_out(int n)
{
    REG(IO_MUX_GPIO(n)) = BIT(12) | 2 << 10;     /* MCU_SEL = GPIO, drive 2 */
    REG(GPIO_FUNC_OUT_SEL(n)) = 0x100;
    REG(GPIO_ENABLE_W1TS) = BIT(n);
}

static void core1_main(void) { bench_run(1); }

int main(void)
{
    uint32_t s0 = systimer_lo(), c0 = ccount();
    while (systimer_lo() - s0 < 16000) {}       /* 1 ms on the XTAL-based timer */
    printf("\nesp-ndf: cpu %lu MHz (measured vs SYSTIMER)\n", (ccount() - c0) / 1000);

    gpio_out(BENCH_PIN(0)); gpio_out(BENCH_PIN(1)); gpio_out(ISR_PIN);
    cpu1_start(core1_main);
    timer_start();
    bench_run(0);
    while (!*(volatile uint32_t *)&bench_res[1].work_per_s) {}
    bench_print(printf, "ESP-NDF (own vectors, CALL0)");
    return 0;
}
