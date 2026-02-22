/* =============================================================================
 * esp32s3.rom.ld  –  ROM function symbols for ESP32-S3 (v0.2 ROM)
 *
 * These symbols let bare-metal code call ROM-resident routines directly
 * without needing to include or compile them.  All addresses are from the
 * ESP32-S3 ROM map (esp32s3_rom.ld distributed with ESP-IDF v5.x).
 *
 * Usage in your linker script:
 *   INCLUDE "arch/xtensa/ld/esp32s3.rom.ld"
 *
 * CAUTION: These are ROM-version specific. Verified against ESP32-S3 ECO3
 *          ROM (marked v0.2 in ESP-IDF).
 * ============================================================================= */

/* ---- UART (used for early boot debug output) ----------------------------- */
/*
 * uart_tx_one_char(uint8_t c)  –  writes one byte to the debug UART
 * On ESP32-S3 this is typically wired to UART0 during ROM operation.
 */
uart_tx_one_char        = 0x40001b58;

/* ---- Basic string / memory routines in ROM ------------------------------ */
/*
 * These are ~4x faster than software implementations because they run from
 * ROM (no cache miss).  Use them in hot paths if IRAM is scarce.
 */
memset  = 0x40001748;
memcpy  = 0x400014b0;
memmove = 0x40001860;
memcmp  = 0x40001590;
strlen  = 0x40001fd0;
strcpy  = 0x400014e8;
strcmp  = 0x40001548;

/* ---- SHA-256 (used by secure boot verification) ------------------------- */
/* erbium_sha256_init / update / final  – available in ROM but rarely useful */

/* ---- Cache management --------------------------------------------------- */
/*
 * Cache_Invalidate_ICache_All()
 * Invalidates the entire I-cache – call before enabling code in flash.
 */
Cache_Invalidate_ICache_All = 0x40003a08;

/*
 * Cache_Invalidate_DCache_All()
 * Invalidates the entire D-cache – call before reading fresh flash data.
 */
Cache_Invalidate_DCache_All = 0x40003a48;

/*
 * Cache_Resume_ICache(autoload)
 * Re-enables the I-cache after it was paused/disabled (e.g. during flash write).
 */
Cache_Resume_ICache = 0x40003a58;

/*
 * Cache_Resume_DCache(autoload)
 */
Cache_Resume_DCache = 0x40003a68;

/* ---- JTAG / OCD stubs --------------------------------------------------- */
/* xt_unhandled_exception – default exception handler in ROM (infinite loop) */
xt_unhandled_exception = 0x40001328;
