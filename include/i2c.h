/* I2C master + slave on the 2 controllers. Open-drain pads, internal pull-ups
 * optional (use external 2.2-4.7k for > 100 kHz on real buses).
 * Master transfers of any length (chunked through the 32-byte FIFO). */
#pragma once
#include "ndf.h"

typedef enum { I2C_OK = 0, I2C_NACK = -1, I2C_TIMEOUT = -2, I2C_ARB_LOST = -3, I2C_ERR = -4 } i2c_err_t;

/* Returns the achieved SCL frequency in Hz, 0 on error. */
uint32_t i2c_master_init(int port, int sda, int scl, uint32_t hz, bool pullups);
/* write wlen bytes, then (repeated start) read rlen bytes. Either may be 0. */
i2c_err_t i2c_master_xfer(int port, uint8_t addr, const uint8_t *w, size_t wlen,
                          uint8_t *r, size_t rlen, uint32_t timeout_us);
/* Clock out a stuck slave (9 SCL pulses + STOP). */
void     i2c_bus_recover(int port);

/* Slave: received bytes go to an internal ring (i2c_slave_read); bytes the
 * master reads come from i2c_slave_write (up to 32 queued in the FIFO). */
int      i2c_slave_init(int port, int sda, int scl, uint8_t addr, bool pullups);
int      i2c_slave_read(int port, uint8_t *buf, size_t n);
int      i2c_slave_write(int port, const uint8_t *buf, size_t n);
