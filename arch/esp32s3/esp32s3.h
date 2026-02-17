/*
 * ESP32-S3 Architecture Definitions
 * Xtensa LX7 Dual-Core
 */

#ifndef ARCH_ESP32S3_H
#define ARCH_ESP32S3_H

#include <stdint.h>

/* CPU Info */
#define ESP32S3_CPU_CORES           2
#define ESP32S3_CPU_FREQ_MHZ        240     /* Max frequency */
#define ESP32S3_ARCH                "xtensa-lx7"

/* Memory Map */
#define ESP32S3_IRAM_BASE           0x40370000
#define ESP32S3_IRAM_SIZE           0x8000      /* 32KB */

#define ESP32S3_DRAM_BASE           0x3FC88000
#define ESP32S3_DRAM_SIZE           0x78000     /* 480KB */

#define ESP32S3_ROM_BASE            0x40000000
#define ESP32S3_ROM_SIZE            0x60000     /* 384KB */

/* Peripheral Base Addresses */
#define DR_REG_SYSTEM_BASE          0x600C0000
#define DR_REG_GPIO_BASE            0x60004000
#define DR_REG_IO_MUX_BASE          0x60009000
#define DR_REG_UART0_BASE           0x60000000
#define DR_REG_UART1_BASE           0x60010000
#define DR_REG_SPI2_BASE            0x60024000
#define DR_REG_SPI3_BASE            0x60025000
#define DR_REG_I2C0_BASE            0x60013000
#define DR_REG_I2C1_BASE            0x60027000
#define DR_REG_TIMG0_BASE           0x6001F000
#define DR_REG_TIMG1_BASE           0x60020000
#define DR_REG_RTC_CNTL_BASE        0x60008000
#define DR_REG_GDMA_BASE            0x6003F000
#define DR_REG_LEDC_BASE            0x60019000
#define DR_REG_APB_SARADC_BASE      0x60040000
#define DR_REG_INTERRUPT_BASE       0x600C2000

/* Interrupt numbers */
#define ESP32S3_INT_UART0           35
#define ESP32S3_INT_GPIO            15
#define ESP32S3_INT_TIMER0          24
#define ESP32S3_INT_TIMER1          25

/* Register access macros */
#define REG_WRITE(addr, val)        (*(volatile uint32_t *)(addr) = (val))
#define REG_READ(addr)              (*(volatile uint32_t *)(addr))
#define REG_SET_BIT(addr, bit)      REG_WRITE(addr, REG_READ(addr) | (bit))
#define REG_CLR_BIT(addr, bit)      REG_WRITE(addr, REG_READ(addr) & ~(bit))
#define REG_SET_FIELD(addr, field, val) \
    REG_WRITE(addr, (REG_READ(addr) & ~(field##_M)) | (((val) << (field##_S)) & (field##_M)))
#define REG_GET_FIELD(addr, field)  \
    ((REG_READ(addr) & (field##_M)) >> (field##_S))

/* Memory barrier */
static inline void memory_barrier(void)
{
    __asm__ volatile ("memw" ::: "memory");
}

/* NOP delay */
static inline void nop_delay(volatile uint32_t count)
{
    while (count--) {
        __asm__ volatile ("nop");
    }
}

#endif /* ARCH_ESP32S3_H */
