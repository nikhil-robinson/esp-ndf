/* I2C driver (TRM ch. 27). Master is polled with a timeout; slave RX is IRQ-fed. */
#include "i2c.h"
#include "gpio.h"
#include "clk.h"
#include "irq.h"
#include "cpu.h"
#include "sys.h"

#if SOC_CPU_RISCV    /* ESP32-P4: same I2C block; the function clock moved to HP_SYS_CLKRST, other base / signals / sources */
#include "soc.h"
#define B(p)            (0x500C4000 + 0x1000 * (p))
#else
#define B(p)            ((p) ? 0x60027000 : 0x60013000)
#endif
#define SCL_LOW(p)      (B(p) + 0x00)
#define CTR(p)          (B(p) + 0x04)   /* b0 sda_force_out b1 scl_force_out b4 master b5 start b8 clk_en b10 fsm_rst b11 upgate */
#define SR(p)           (B(p) + 0x08)   /* b13:8 rx cnt b23:18 tx cnt b4 busy */
#define TO(p)           (B(p) + 0x0C)
#define SLV_ADDR(p)     (B(p) + 0x10)
#define FIFO_CONF(p)    (B(p) + 0x18)   /* b4:0 rx wm b9:5 tx wm b12 rx rst b13 tx rst */
#define DATA(p)         (B(p) + 0x1C)
#define INT_RAW(p)      (B(p) + 0x20)
#define INT_CLR(p)      (B(p) + 0x24)
#define INT_ENA(p)      (B(p) + 0x28)
#define INT_ST(p)       (B(p) + 0x2C)
#define SDA_HOLD(p)     (B(p) + 0x30)
#define SDA_SAMPLE(p)   (B(p) + 0x34)
#define SCL_HIGH(p)     (B(p) + 0x38)   /* b8:0 high b15:9 wait_high */
#define START_HOLD(p)   (B(p) + 0x40)
#define RSTART_SETUP(p) (B(p) + 0x44)
#define STOP_HOLD(p)    (B(p) + 0x48)
#define STOP_SETUP(p)   (B(p) + 0x4C)
#define FILTER(p)       (B(p) + 0x50)
#if !SOC_CPU_RISCV
#define CLK_CONF(p)     (B(p) + 0x54)   /* b7:0 div-1 b20 sel (0 = XTAL) b21 active */
#endif
#define COMD(p, i)      (B(p) + 0x58 + 4 * (i))

/* INT bits */
#define I_RXWM     BIT(0)
#define I_END      BIT(3)
#define I_ARB      BIT(5)
#define I_DONE     BIT(7)
#define I_TOUT     BIT(8)
#define I_NACK     BIT(10)

enum { OP_WRITE = 1, OP_STOP = 2, OP_READ = 3, OP_END = 4, OP_RSTART = 6 };
#define CMD(op, n, ackchk, ackval) ((n) | (ackchk) << 8 | (ackval) << 10 | (op) << 11)

#if SOC_CPU_RISCV
static const uint8_t sig_sda[2] = { 69, 71 }, sig_scl[2] = { 68, 70 };
#define SLAVE_SRC(p)    (SRC_I2C0 + (p))
/* function clock XTAL 40 MHz with a divider in HP_SYS_CLKRST: I2C0 PERI_CLK_CTRL10 (src b0, en b1, div-1 b9:2),
 * I2C1 src b26 / en b27 in PERI_CLK_CTRL10 and div-1 b7:0 in PERI_CLK_CTRL11 (hw_ver3 hp_sys_clkrst_reg.h) */
static void sclk_set(int p, uint32_t div)
{
    if (!p) { REG_FIELD(HP_CLKRST_BASE + 0x40, 2, 8, div - 1); REG_FIELD(HP_CLKRST_BASE + 0x40, 0, 1, 0); REG_SET(HP_CLKRST_BASE + 0x40, BIT(1)); }
    else { REG_FIELD(HP_CLKRST_BASE + 0x44, 0, 8, div - 1); REG_CLR(HP_CLKRST_BASE + 0x40, BIT(26)); REG_SET(HP_CLKRST_BASE + 0x40, BIT(27)); }
}
static uint32_t sclk_div(int p) { return (p ? REG(HP_CLKRST_BASE + 0x44) & 0xFF : (REG(HP_CLKRST_BASE + 0x40) >> 2) & 0xFF) + 1; }
#else
static const uint8_t sig_sda[2] = { 90, 92 }, sig_scl[2] = { 89, 91 };
#define SLAVE_SRC(p)    ((p) ? 43 : 42)
static void sclk_set(int p, uint32_t div) { REG(CLK_CONF(p)) = (div - 1) | BIT(21); }
static uint32_t sclk_div(int p) { return (REG(CLK_CONF(p)) & 0xFF) + 1; }
#endif
static const periph_t pid[2] = { PERIPH_I2C0, PERIPH_I2C1 };
static uint8_t pins[2][2];
static struct { uint32_t hz, filt, to; bool pu; } mcfg[2];   /* to rebuild the master after an error */

static void pads(int p, int sda, int scl, bool pu)
{
    uint32_t f = GPIO_OUTPUT | GPIO_INPUT | GPIO_OPENDRAIN | (pu ? GPIO_PULLUP : 0);
    gpio_config(sda, f); gpio_config(scl, f);
    gpio_write(sda, 1); gpio_write(scl, 1);
    gpio_out_signal(sda, sig_sda[p], false); gpio_in_signal(sda, sig_sda[p], false);
    gpio_out_signal(scl, sig_scl[p], false); gpio_in_signal(scl, sig_scl[p], false);
    pins[p][0] = sda; pins[p][1] = scl;
}

static void fifo_reset(int p)
{
    REG_SET(FIFO_CONF(p), BIT(12) | BIT(13));
    REG_CLR(FIFO_CONF(p), BIT(12) | BIT(13));
}

uint32_t i2c_master_init(int p, int sda, int scl, uint32_t hz, bool pu)
{
    if (!(p >= 0 && p < 2)) return 0;      /* caller index: never past the hardware */
    if (p < 0 || p > 1 || hz < 1000 || hz > 1000000) return 0;
    mcfg[p].hz = hz; mcfg[p].pu = pu; mcfg[p].filt = BIT(8) | BIT(9) | 0x77; mcfg[p].to = BIT(5) | 23;
    periph_enable(pid[p]);
    periph_reset(pid[p]);                                   /* as IDF: no state left from an earlier role */
    REG(INT_ENA(p)) = 0; REG(INT_CLR(p)) = ~0u;             /* a slave ISR may still be attached to this port */
    /* source XTAL 40 MHz; integer pre-divider keeps half a bus period <= 511 */
    uint32_t div = 40000000 / (hz * 1024) + 1;
    uint32_t sclk = 40000000 / div, half = sclk / hz / 2;
    uint32_t wait = hz >= 80000 ? half / 2 - 2 : half / 4;
    sclk_set(p, div);
    REG(CTR(p)) = BIT(4) | BIT(8) | BIT(9) | BIT(0) | BIT(1);   /* master, clk on, arbitration (reset default), open-drain */
    REG(SCL_LOW(p)) = half - 1;
    REG(SCL_HIGH(p)) = (half - wait) | wait << 9;
    REG(SDA_HOLD(p)) = half / 4 - 1;
    REG(SDA_SAMPLE(p)) = half / 2 - 1;
    REG(RSTART_SETUP(p)) = half - 1; REG(STOP_SETUP(p)) = half - 1;
    REG(START_HOLD(p)) = half - 1;   REG(STOP_HOLD(p)) = half - 1;
    REG(TO(p)) = mcfg[p].to;                                /* SCL-held timeout, default 2^23 sclk cycles */
    REG(FILTER(p)) = mcfg[p].filt;                          /* glitch filter on both lines */
    REG(FIFO_CONF(p)) = BIT(14);                            /* fifo_prt_en: watermark/ovf logic */
    fifo_reset(p);
    REG_SET(CTR(p), BIT(11));                               /* latch timing config */
    pads(p, sda, scl, pu);
    REG(INT_ENA(p)) = 0;
    return sclk / (2 * half);
}

void i2c_bus_recover(int p)
{
    /* IDF s_i2c_master_clear_bus: SCL low first (no START), clock while SDA is held, then STOP */
    int sda = pins[p][0], scl = pins[p][1];
    gpio_config(scl, GPIO_OUTPUT | GPIO_INPUT | GPIO_OPENDRAIN | GPIO_PULLUP);
    gpio_config(sda, GPIO_OUTPUT | GPIO_INPUT | GPIO_OPENDRAIN | GPIO_PULLUP);
    gpio_clr(scl); gpio_set(sda); delay_us(5);
    for (int i = 0; i < 9 && !gpio_read(sda); i++) {
        gpio_set(scl); delay_us(5); gpio_clr(scl); delay_us(5);
    }
    gpio_clr(sda); gpio_set(scl); delay_us(5); gpio_set(sda);   /* STOP */
    REG_SET(CTR(p), BIT(10)); REG_CLR(CTR(p), BIT(10));          /* FSM reset */
    pads(p, sda, scl, true);
}

/* run the queued commands; wait for END (chunk boundary) or DONE */
static i2c_err_t run(int p, uint32_t until, uint32_t t0, uint32_t lim)
{
    REG(INT_CLR(p)) = ~0u;
    REG_SET(CTR(p), BIT(11));
    REG_SET(CTR(p), BIT(5));
    for (;;) {
        uint32_t r = REG(INT_RAW(p));
        if (r & I_NACK) return I2C_NACK;
        if (r & I_ARB)  return I2C_ARB_LOST;
        if (r & I_TOUT) return I2C_TIMEOUT;
        if (r & until)  return I2C_OK;
        if (ccount() - t0 > lim) return I2C_TIMEOUT;
    }
}

/* wait for the other master's frame to end after a lost arbitration (bounded by the caller's timeout) */
static void wait_idle(int p, uint32_t t0, uint32_t lim)
{
    while ((REG(SR(p)) & BIT(4)) && ccount() - t0 < lim) {}
}

i2c_err_t i2c_master_xfer(int p, uint16_t addr, const uint8_t *w, size_t wl,
                          uint8_t *r, size_t rl, uint32_t us)
{
    if (!(p >= 0 && p < 2)) return -1;      /* caller index: never past the hardware */
    /* 10-bit (IDF i2c_master.c): header 11110 A9 A8 R/W, then A7..A0; a read sends the write header,
     * repeated start, then the header byte alone with R = 1 */
    bool ten = addr & I2C_ADDR10;
    uint8_t hdr = ten ? (uint8_t)(0x78 | ((addr >> 8) & 3)) : (uint8_t)(addr & 0x7F);
    uint32_t t0 = ccount(), lim = us * (sys_cpu_hz() / 1000000);
    i2c_err_t e = I2C_OK;
    fifo_reset(p);
    int c = 0;
    REG(COMD(p, c++)) = CMD(OP_RSTART, 0, 0, 0);
    if (wl || !rl || ten) {                                  /* write phase (or address probe / 10-bit read) */
        REG(DATA(p)) = hdr << 1;
        if (ten) REG(DATA(p)) = (uint8_t)addr;
        size_t room = ten ? 30 : 31, sent = 0;
        size_t n = wl < room ? wl : room;
        for (size_t i = 0; i < n; i++) REG(DATA(p)) = w[i];
        REG(COMD(p, c++)) = CMD(OP_WRITE, n + 1 + ten, 1, 0);
        sent = n;
        while (sent < wl) {                                  /* more than the FIFO holds */
            REG(COMD(p, c)) = CMD(OP_END, 0, 0, 0);
            if ((e = run(p, I_END, t0, lim))) goto out;
            n = wl - sent < 32 ? wl - sent : 32;
            for (size_t i = 0; i < n; i++) REG(DATA(p)) = w[sent + i];
            sent += n;
            c = 0;
            REG(COMD(p, c++)) = CMD(OP_WRITE, n, 1, 0);
        }
        if (rl) REG(COMD(p, c++)) = CMD(OP_RSTART, 0, 0, 0);
    }
    if (rl) {
        REG(DATA(p)) = hdr << 1 | 1;
        REG(COMD(p, c++)) = CMD(OP_WRITE, 1, 1, 0);
        size_t got = 0;
        while (rl - got > 32) {                              /* full 32-byte chunks, all ACKed */
            REG(COMD(p, c++)) = CMD(OP_READ, 32, 0, 0);
            REG(COMD(p, c)) = CMD(OP_END, 0, 0, 0);
            if ((e = run(p, I_END, t0, lim))) goto out;
            for (int i = 0; i < 32; i++) r[got++] = REG(DATA(p));
            c = 0;
        }
        size_t last = rl - got;
        if (last > 1) REG(COMD(p, c++)) = CMD(OP_READ, last - 1, 0, 0);
        REG(COMD(p, c++)) = CMD(OP_READ, 1, 0, 1);           /* NACK the final byte */
        REG(COMD(p, c++)) = CMD(OP_STOP, 0, 0, 0);
        if ((e = run(p, I_DONE, t0, lim))) goto out;
        for (size_t i = 0; i < last; i++) r[got++] = REG(DATA(p));
        return I2C_OK;
    }
    REG(COMD(p, c++)) = CMD(OP_STOP, 0, 0, 0);
    e = run(p, I_DONE, t0, lim);
out:
    if (e) {                                                 /* leave the bus idle and the controller clean */
        REG_SET(CTR(p), BIT(10)); REG_CLR(CTR(p), BIT(10));
        if (e == I2C_ARB_LOST) wait_idle(p, t0, lim);         /* the winner's frame: caller may retry */
        else i2c_bus_recover(p);                              /* IDF: clear the bus (STOP) */
        /* IDF on S3 (no HW FSM reset): reset the whole module and restore timing/filter; an fsm_rst alone
         * leaves state that makes the next 10-bit header NACK */
        uint32_t f = mcfg[p].filt, to = mcfg[p].to;
        i2c_master_init(p, pins[p][0], pins[p][1], mcfg[p].hz, mcfg[p].pu);
        mcfg[p].filt = f; mcfg[p].to = to;
        REG(FILTER(p)) = f; REG(TO(p)) = to; REG_SET(CTR(p), BIT(11));
    }
    return e;
}

/* ---- slave ---------------------------------------------------------------- */
#define SRING 256
static uint8_t srx[2][SRING];
static volatile uint32_t sh[2], st_[2];

RAM_FN static void slave_isr(void *arg)
{
    int p = (int)arg;
    uint32_t s = REG(INT_ST(p));
    uint32_t n = (REG(SR(p)) >> 8) & 0x3F;
    while (n--) {
        uint8_t b = REG(DATA(p));
        if (sh[p] - st_[p] < SRING) srx[p][sh[p]++ & (SRING - 1)] = b;
    }
    REG(INT_CLR(p)) = s;
}

int i2c_slave_init(int p, int sda, int scl, uint16_t addr, bool pu)
{
    if (!(p >= 0 && p < 2)) return -1;      /* caller index: never past the hardware */
    if (p < 0 || p > 1) return -1;
    periph_enable(pid[p]);
    periph_reset(pid[p]);                                   /* as IDF: no state left from an earlier role */
    sclk_set(p, 1);
    bool ten = addr & I2C_ADDR10;
    /* as IDF's slave: open-drain outs, rx_full_ack_level, slv_tx_auto_start, (10-bit r/w check) */
    REG(INT_ENA(p)) = 0;
    REG(CTR(p)) = BIT(0) | BIT(1) | BIT(3) | BIT(12) | (ten ? BIT(13) : 0);
    REG_SET(CTR(p), BIT(10)); REG_CLR(CTR(p), BIT(10));     /* FSM reset: a re-init may find it mid-frame */
    /* 10-bit as IDF i2c_ll_set_slave_addr: [14:7] = A7..A0, [6:0] = 0x78 | A9 A8 */
    REG(SLV_ADDR(p)) = ten ? BIT(31) | (addr & 0xFFu) << 7 | (0x78 | ((addr >> 8) & 3)) : (addr & 0x7F);
    REG(SDA_HOLD(p)) = 10; REG(SDA_SAMPLE(p)) = 10;
    REG(FILTER(p)) = BIT(8) | BIT(9) | 0x77;                /* SCL and SDA thresholds (IDF 0x377) */
    REG(FIFO_CONF(p)) = BIT(14) | 16;                       /* fifo_prt_en, rx watermark 16 */
    fifo_reset(p);
    REG_SET(CTR(p), BIT(11));
    pads(p, sda, scl, pu);
    sh[p] = st_[p] = 0;
    REG(INT_CLR(p)) = ~0u;
    if (irq_attach(SLAVE_SRC(p), 1, slave_isr, (void *)p) < 0) return -1;
    REG(INT_ENA(p)) = I_RXWM | I_DONE;
    return 0;
}

int i2c_slave_read(int p, uint8_t *b, size_t n)
{
    if (!(p >= 0 && p < 2)) return -1;      /* caller index: never past the hardware */
    size_t k = 0;
    while (k < n && st_[p] != sh[p]) b[k++] = srx[p][st_[p]++ & (SRING - 1)];
    return k;
}

int i2c_slave_write(int p, const uint8_t *b, size_t n)
{
    if (!(p >= 0 && p < 2)) return -1;      /* caller index: never past the hardware */
    size_t k = 0;
    while (k < n && ((REG(SR(p)) >> 18) & 0x3F) < 32) REG(DATA(p)) = b[k++];
    return k;
}

void i2c_set_filter(int p, uint32_t cycles)
{
    if (!(p >= 0 && p < 2)) return;      /* caller index: never past the hardware */
    uint32_t c = cycles > 15 ? 15 : cycles;
    REG(FILTER(p)) = mcfg[p].filt = cycles ? BIT(8) | BIT(9) | c << 4 | c : 0;    /* SCL [3:0], SDA [7:4] */
    REG_SET(CTR(p), BIT(11));
}

uint32_t i2c_set_timeout(int p, uint32_t us)
{
    if (!(p >= 0 && p < 2)) return 0;      /* caller index: never past the hardware */
    /* TO counts 2^n source cycles (n 1..22) of SCL low/held; pick the smallest >= us */
    uint32_t sclk = 40000000 / sclk_div(p), n = 1;
    while (n < 22 && ((uint64_t)1 << n) * 1000000 < (uint64_t)us * sclk) n++;
    REG(TO(p)) = mcfg[p].to = BIT(5) | n;
    REG_SET(CTR(p), BIT(11));
    return (uint32_t)(((uint64_t)1 << n) * 1000000 / sclk);
}
