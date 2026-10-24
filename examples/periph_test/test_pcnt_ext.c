#include "test.h"
#include "pcnt.h"
#include "gpio.h"
#include "sys.h"

/* CPU-made quadrature: A on 12 -> jumper -> 13, B on 10 -> jumper -> 11; index Z on pad 14 (in+out). */
static int st;
static void quad(int n)                                    /* n > 0 forward, < 0 back, one x4 step each */
{
    static const uint8_t seq[4] = { 0, 1, 3, 2 };          /* A = bit0, B = bit1 */
    for (int i = 0; i < (n > 0 ? n : -n); i++) {
        st = (st + (n > 0 ? 1 : 3)) & 3;
        gpio_write(12, seq[st] & 1); gpio_write(10, seq[st] >> 1);
        delay_us(2);
    }
}

static volatile int nev, last_ev; static volatile int32_t last_at, steps[64]; static volatile int nsteps;
static void on_watch(int u, uint32_t ev, int32_t at, void *a) { (void)u; (void)a; nev++; last_ev = ev; last_at = at; }
static void on_step(int u, uint32_t ev, int32_t at, void *a) { (void)u; (void)ev; (void)a; if (nsteps < 64) steps[nsteps++] = at; }

void test_pcnt_ext(void)
{
    __asm__ volatile("rsil a2, 0" ::: "a2");
    gpio_config(12, GPIO_OUTPUT); gpio_config(10, GPIO_OUTPUT); gpio_write(12, 0); gpio_write(10, 0);
    st = 0;
    pcnt_quadrature(1, 13, 11);
    pcnt_clear(1);
    quad(100);
    int32_t f = pcnt_get(1);
    quad(-40);
    int32_t b = pcnt_get(1);
    printf("    quadrature: +100 -> %ld, -40 -> %ld\n", f, b);
    CHECK(f == 100 && b == 60);

    pcnt_clear(1); nev = 0;
    pcnt_watch(1, 25, -10, PCNT_EV_THRES0 | PCNT_EV_THRES1 | PCNT_EV_ZERO, on_watch, 0);
    quad(30);
    int e1 = nev, ev1 = last_ev; int32_t a1 = last_at;
    quad(-30);                                             /* 30 -> 0 (zero) */
    int e2 = nev;
    quad(-12);                                             /* -> -12 crosses -10 */
    int ev3 = last_ev; int32_t a3 = last_at;
    printf("    watch: thres0=25 -> %d event (ev %d at %ld); back to 0 -> %d total; thres1=-10 -> ev %d at %ld\n",
           e1, ev1, a1, e2, ev3, a3);
    CHECK(e1 == 1 && ev1 == PCNT_EV_THRES0 && a1 == 25 && e2 >= 2 && ev3 == PCNT_EV_THRES1 && a3 == -10);
    pcnt_watch(1, 0, 0, 0, 0, 0);

    pcnt_clear(1); nsteps = 0;
    CHECK(pcnt_step_notify(1, 10, on_step, 0) == 0);
    quad(35); quad(-50);                                   /* 10 20 30, (35->) 20 10 0 -10 */
    printf("    step 10: %d calls:", nsteps);
    for (int i = 0; i < nsteps; i++) printf(" %ld", steps[i]);
    printf("\n");
    static const int32_t want[] = { 10, 20, 30, 20, 10, 0, -10 };
    bool sok = nsteps == 7;
    for (int i = 0; sok && i < 7; i++) sok = steps[i] == want[i];
    CHECK(sok);
    pcnt_watch(1, 0, 0, 0, 0, 0);

    gpio_config(14, GPIO_OUTPUT | GPIO_INPUT); gpio_write(14, 0);
    pcnt_clear(1);
    pcnt_index(1, 14, true);
    quad(77);
    gpio_write(14, 1); delay_us(5); gpio_write(14, 0);
    int32_t lat = pcnt_index_count(1);
    quad(5);
    int32_t after = pcnt_get(1);
    printf("    index: latched %ld at Z, count after Z +5 = %ld\n", lat, after);
    CHECK(lat == 77 && after == 5);
    gpio_irq_detach(14);
}
