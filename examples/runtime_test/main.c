/* Runtime pieces through the on-device test runner: events (ISR, other core, overflow), the command line,
 * cooperative tasks; timings next to the IDF sibling bench/idf_runtime (esp_event, FreeRTOS yield). */
#include "unit.h"
#include "event.h"
#include "cmd.h"
#include "coop.h"
#include "console.h"
#include "gptimer.h"
#include "sys.h"
#include "xtensa.h"
#include <string.h>
#include <stdlib.h>

/* ---- events ---- */
static volatile uint32_t got_n, got_sum, any_n;
static void on_tick(uint16_t id, uint32_t d, void *a) { (void)id; (void)a; got_n++; got_sum += d; }
static void on_any(uint16_t id, uint32_t d, void *a) { (void)id; (void)d; (void)a; any_n++; }
static volatile uint32_t isr_k;
static void tmr_isr(void *a) { (void)a; event_post(1, ++isr_k); }

TEST(event_isr_post)
{
    got_n = got_sum = any_n = 0; isr_k = 0;
    event_on(1, on_tick, 0); event_on(EVENT_ANY, on_any, 0);
    gpt_init(0, 1000000); gpt_set(0, 0);
    gpt_alarm(0, 1000, true, tmr_isr, 0, 1);               /* 1 kHz */
    gpt_start(0);
    uint32_t t0 = time_ms();
    while (time_ms() - t0 < 50) event_dispatch();
    gpt_alarm_off(0); gpt_stop(0);
    event_dispatch();
    UASSERT(got_n >= 48 && got_n <= 52);
    UASSERT_EQ(got_sum, (uint64_t)got_n * (got_n + 1) / 2);   /* every value once, in order of sum */
    UASSERT_EQ(any_n, got_n);
    event_off(on_tick); event_off(on_any);
}

static volatile int c1_done;
static void core1_poster(void) { for (int i = 0; i < 1000; i++) while (event_post(2, 1)) {} c1_done = 1; for (;;) {} }
TEST(event_other_core)
{
    got_n = got_sum = 0; c1_done = 0;
    event_on(2, on_tick, 0);
    cpu1_start(core1_poster);
    uint32_t t0 = time_ms();
    while ((!c1_done || got_n < 1000) && time_ms() - t0 < 1000) event_dispatch();
    UASSERT_EQ(got_n, 1000);                             /* full-queue attempts are counted as drops, then retried */
    event_off(on_tick);
}

TEST(event_full_counted)
{
    uint32_t d0 = event_dropped();
    int ok = 0;
    for (int i = 0; i < NDF_EVENT_QUEUE + 10; i++) ok += event_post(9, i) == 0;
    UASSERT_EQ(ok, NDF_EVENT_QUEUE);
    UASSERT_EQ(event_dropped() - d0, 10);
    UASSERT_EQ(event_dispatch(), NDF_EVENT_QUEUE);
}

TEST(event_cost)
{
    event_on(3, on_tick, 0);
    uint32_t best_p = ~0u, best_d = ~0u;
    for (int i = 0; i < 16; i++) {
        uint32_t c = ccount(); event_post(3, 1); uint32_t p = ccount() - c;
        c = ccount(); event_dispatch(); uint32_t d = ccount() - c;
        if (p < best_p) best_p = p;
        if (d < best_d) best_d = d;
    }
    printf("      post %lu cycles, dispatch one event to one handler %lu cycles\n", (unsigned long)best_p, (unsigned long)best_d);
    event_off(on_tick);
}

/* ---- command line ---- */
static int last_argc; static char last_args[64];
static int c_echo(int argc, char **argv)
{
    last_argc = argc; last_args[0] = 0;
    for (int i = 1; i < argc; i++) { strcat(last_args, argv[i]); if (i + 1 < argc) strcat(last_args, "|"); }
    return 7;
}
CMD(echo, "print the arguments", c_echo);
static int c_add(int argc, char **argv) { if (argc != 3) return -2; printf("%d\n", atoi(argv[1]) + atoi(argv[2])); return 0; }
CMD(add, "add two numbers", c_add);

TEST(cmd_parse)
{
    char l1[] = "  echo one \"two words\" 'three'  ";
    UASSERT_EQ(cmd_exec(l1), 7);
    UASSERT_EQ(last_argc, 4);
    UASSERT(!strcmp(last_args, "one|two words|three"));
    char l2[] = "nosuch x";
    UASSERT_EQ(cmd_exec(l2), -1);
    char l3[] = "   ";
    UASSERT_EQ(cmd_exec(l3), 0);
    char l4[] = "add 1";
    UASSERT_EQ(cmd_exec(l4), -2);
}

/* ---- cooperative tasks ---- */
static volatile int pp[2], order_ok = 1, last_task = -1;
static void pinger(void *a)
{
    int id = (int)a;
    for (int i = 0; i < 100; i++) {
        if (last_task == id) order_ok = 0;                 /* strict alternation between the two tasks */
        last_task = id; pp[id]++;
        coop_yield();
    }
}
static uint32_t sw_cycles; static volatile uint32_t mark;
static void timer_a(void *a) { (void)a; for (int i = 0; i < 64; i++) { mark = ccount(); coop_yield(); } }
static void timer_b(void *a) { (void)a; uint32_t best = ~0u; for (int i = 0; i < 64; i++) { uint32_t d = ccount() - mark; if (i && d < best) best = d; coop_yield(); } sw_cycles = best; }
static volatile uint32_t woke_ms;
static void sleeper(void *a) { (void)a; uint32_t t = time_ms(); coop_sleep_ms(20); woke_ms = time_ms() - t; }

static uint8_t st[4][2048] __attribute__((aligned(16)));
TEST(coop_tasks)
{
    pp[0] = pp[1] = 0; order_ok = 1; last_task = -1;
    UASSERT(coop_spawn(pinger, (void *)0, st[0], sizeof st[0]) >= 0);
    UASSERT(coop_spawn(pinger, (void *)1, st[1], sizeof st[1]) >= 0);
    UASSERT(coop_spawn(sleeper, 0, st[2], sizeof st[2]) >= 0);
    coop_run();
    UASSERT_EQ(pp[0], 100); UASSERT_EQ(pp[1], 100);
    UASSERT(order_ok);
    UASSERT(woke_ms >= 20 && woke_ms <= 21);
    coop_spawn(timer_a, 0, st[0], sizeof st[0]);
    coop_spawn(timer_b, 0, st[1], sizeof st[1]);
    coop_run();
    printf("      task -> scheduler -> task switch %lu cycles; sleep 20 ms woke after %lu ms\n", (unsigned long)sw_cycles, (unsigned long)woke_ms);
}

int main(void)
{
    int f = unit_run(NULL);
    printf("runtime_test %s\n", f ? "FAILED" : "done");
    printf("> ");
    for (;;) cmd_poll();                                    /* interactive: tools/repl_test.py */
}
