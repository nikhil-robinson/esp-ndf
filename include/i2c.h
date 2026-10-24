/* I2C master + slave on the 2 controllers. Open-drain pads, internal pull-ups
 * optional (use external 2.2-4.7k for > 100 kHz on real buses).
 * Master transfers of any length (chunked through the 32-byte FIFO). */
#pragma once
#include "ndf.h"

typedef enum { I2C_OK = 0, I2C_NACK = -1, I2C_TIMEOUT = -2, I2C_ARB_LOST = -3, I2C_ERR = -4 } i2c_err_t;

/* 7-bit addresses as is; OR in I2C_ADDR10 for a 10-bit address (master and slave). */
#define I2C_ADDR10 0x8000u

/* Returns the achieved SCL frequency in Hz, 0 on error. */
uint32_t i2c_master_init(int port, int sda, int scl, uint32_t hz, bool pullups);
/* write wlen bytes, then (repeated start) read rlen bytes. Either may be 0.
 * I2C_ARB_LOST (multi-master): returned after the winner's frame ended; retry. */
i2c_err_t i2c_master_xfer(int port, uint16_t addr, const uint8_t *w, size_t wlen,
                          uint8_t *r, size_t rlen, uint32_t timeout_us);
/* Glitch filter on SDA/SCL: pulses shorter than `cycles` APB cycles are ignored (0 = off, max 15). */
void     i2c_set_filter(int port, uint32_t cycles);
/* SCL-held / stretch timeout: rounded up to a power of 2 of the source clock; returns the value set (us). */
uint32_t i2c_set_timeout(int port, uint32_t us);
/* Clock out a stuck slave (9 SCL pulses + STOP, as IDF). */
void     i2c_bus_recover(int port);

/* Slave: received bytes go to an internal ring (i2c_slave_read); bytes the
 * master reads come from i2c_slave_write (up to 32 queued in the FIFO). */
int      i2c_slave_init(int port, int sda, int scl, uint16_t addr, bool pullups);
int      i2c_slave_read(int port, uint8_t *buf, size_t n);
int      i2c_slave_write(int port, const uint8_t *buf, size_t n);
