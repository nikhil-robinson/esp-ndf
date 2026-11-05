#include "test.h"
#include "uart.h"
#include "gpio.h"
#include "sys.h"
#include <string.h>

/* UART1 TX 12 -> jumper -> RX 13. RTS/DE on pad 16 (CTS read back from the same pad). */
#define U1_INT_ENA 0x6001000C
#define U1_STATUS  0x6001001C
#define U1_INT_RAW 0x60010004
#define GPIO_IN    0x6000403C
static uint8_t tx[512], rx[512];

static int rx_all(uint8_t *b, int n, uint32_t ms)
{
    int k = 0; uint32_t t0 = ccount();
    while (k < n && ccount() - t0 < ms * (sys_cpu_hz() / 1000)) k += uart_read(1, b + k, n - k);
    return k;
}

static void flow(void)
{
    uart_cfg_t c = { .baud = 115200, .tx_pin = 12, .rx_pin = 13 };
    uart_init(1, &c);
    uart_set_flow(1, 16, 16, 64);
    for (int i = 0; i < 300; i++) tx[i] = (uint8_t)(i * 7 + 1);
    REG(U1_INT_ENA) = 0;                                       /* RX ISR off: only the 128 B FIFO */
    uart_write(1, tx, 300);
    uint32_t t0 = ccount(); while (ccount() - t0 < (sys_cpu_hz() / 1000) * 60) {}   /* 300 B would take 26 ms */
    uint32_t inf = REG(U1_STATUS) & 0x3FF, ovf = REG(U1_INT_RAW) & BIT(4);
    REG(U1_INT_ENA) = BIT(0) | BIT(8) | BIT(4) | BIT(1);       /* RX + TX-empty (TX ring still has 172 B) */
    int n = rx_all(rx, 300, 200);
    printf("    flow: RX FIFO held at %lu (RTS at 64), FIFO overflow %s, then %d/300 B, %s\n", inf, ovf ? "YES" : "no",
           n, memcmp(tx, rx, 300) ? "BAD" : "ok");
    CHECK(inf >= 64 && inf < 128 && !ovf && n == 300 && !memcmp(tx, rx, 300));
    uart_set_flow(1, -1, -1, 0);
}

static void rs485(void)
{
    uart_cfg_t c = { .baud = 1000000, .tx_pin = 12, .rx_pin = 13 };
    uart_init(1, &c);
    uart_set_mode(1, UART_MODE_RS485, 16);
    for (int i = 0; i < 32; i++) tx[i] = (uint8_t)(0xA0 + i);
    bool de_idle = !(REG(GPIO_IN) & BIT(16));
    uart_write(1, tx, 32);
    bool de_tx = REG(GPIO_IN) & BIT(16);
    /* turnaround: last TX edge -> DE low, minus the stop bit */
    uint32_t last = ccount(), t0 = last, tde = 0, prev = REG(GPIO_IN);
    while (ccount() - t0 < (sys_cpu_hz() / 1000) * 2) {
        uint32_t v = REG(GPIO_IN), now = ccount();
        if ((v ^ prev) & BIT(12)) last = now;
        if (!tde && (prev & BIT(16)) && !(v & BIT(16))) { tde = now; break; }
        prev = v;
    }
    int echo = rx_all(rx, 32, 2);
    uint32_t turn = tde ? tde - last : 0;
    printf("    RS485: DE idle %s, high while sending %s, low after %lu cycles from the last edge (stop bit 240), echo %d B\n",
           de_idle ? "low" : "HIGH", de_tx ? "yes" : "NO", turn, echo);
    CHECK(de_idle && de_tx && tde && echo == 0);
    METRIC("RS485 DE release after stop bit", "%lu cycles", turn > 240 ? turn - 240 : 0);
}

static void collision(void)
{
    uart_cfg_t c = { .baud = 115200, .tx_pin = 12, .rx_pin = 13 };
    uart_init(1, &c);
    uart_set_mode(1, UART_MODE_RS485_CD, -1);
    for (int i = 0; i < 64; i++) tx[i] = (uint8_t)(i * 3);
    uart_write(1, tx, 64);
    int n = rx_all(rx, 64, 20);
    uint32_t cl0 = uart_rs485_clashes(1);
    /* drive UART1 RX from UART2 (pad 17) sending other bytes at the same time */
    uart_cfg_t c2 = { .baud = 115200, .tx_pin = 17, .rx_pin = -1 };
    uart_init(2, &c2);
    gpio_in_signal(17, 15, false);                             /* U1RXD <- pad 17 */
    static uint8_t other[64];
    for (int i = 0; i < 64; i++) other[i] = (uint8_t)~tx[i];
    uart_write(2, other, 64); uart_write(1, tx, 64);
    uart_flush(1); uart_flush(2);
    uint32_t cl1 = uart_rs485_clashes(1);
    printf("    RS485 collision detect: own echo %d/64 B %s, clashes %lu; with a 2nd driver: clashes %lu\n",
           n, memcmp(tx, rx, 64) ? "BAD" : "ok", cl0, cl1);
    CHECK(n == 64 && !memcmp(tx, rx, 64) && cl0 == 0 && cl1 > 0);
    uart_set_mode(1, UART_MODE_UART, -1);
}

static void irda(void)                                    /* half duplex: UART2 IrDA TX (pad 17) -> UART1 IrDA RX */
{
    uart_cfg_t c = { .baud = 115200, .tx_pin = -1, .rx_pin = 13 }, c2 = { .baud = 115200, .tx_pin = 17, .rx_pin = -1 };
    uart_init(1, &c); uart_init(2, &c2);
    gpio_in_signal(17, 15, false);                             /* U1RXD <- pad 17 */
    uart_set_mode(1, UART_MODE_IRDA, -1); uart_set_mode(2, UART_MODE_IRDA, -1);
    for (int i = 0; i < 64; i++) tx[i] = (uint8_t)(i * 13 + 5);
    uart_write(2, tx, 64);
    int n = rx_all(rx, 64, 20);
    printf("    IrDA UART2 -> UART1: %d/64 B %s\n", n, memcmp(tx, rx, 64) ? "BAD" : "ok");
    CHECK(n == 64 && !memcmp(tx, rx, 64));
    uart_set_mode(1, UART_MODE_UART, -1); uart_set_mode(2, UART_MODE_UART, -1);
}

static void pattern(void)
{
    uart_cfg_t c = { .baud = 115200, .tx_pin = 12, .rx_pin = 13 };
    uart_init(1, &c);
    uart_set_pattern(1, '+', 3, 9, 0, 0);                      /* IDF example values */
    const char *m = "hello+++world";
    uart_write(1, m, 13);
    uint32_t t0 = ccount(); while (ccount() - t0 < (sys_cpu_hz() / 1000) * 3) {}  /* 13 B = 1.1 ms */
    int pos = uart_pattern_pop(1), none = uart_pattern_pop(1);
    int n = rx_all(rx, 13, 20);
    printf("    pattern '+++': %d B, at offset %d (5), next %d\n", n, pos, none);
    CHECK(n == 13 && pos == 5 && none == -1);
    uart_set_pattern(1, 0, 0, 0, 0, 0);
}

void test_uart_modes(void)
{
    irq_enable_all();
    flow();
    rs485();
    collision();
    irda();
    pattern();
}

NDF_UNIT(020, uart_modes);
