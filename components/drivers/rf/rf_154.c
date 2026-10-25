/* 4.3: IEEE 802.15.4 O-QPSK (2.4 GHz PHY) transmit by DAC waveform. The ESP32-S3 has no
 * 802.15.4 radio, so we synthesise the baseband and play it through the TX DAC (40 MS/s).
 * O-QPSK, 2 Mchip/s, 32-chip DSSS per 4-bit symbol, half-sine pulse shaping, Q offset half a chip.
 * Transmit only (no 15.4 receiver here); verify by IQ capture on the sibling board. */
#include "rf.h"
#include <math.h>

/* IEEE 802.15.4-2006 Table 24: 4-bit symbol -> 32 chips, c0 in bit 0 (LSB first). */
static const uint32_t pn[16] = {
    0xD9C3522Eu, 0xED9C3522u, 0x2ED9C352u, 0x22ED9C35u,
    0x522ED9C3u, 0x3522ED9Cu, 0xC3522ED9u, 0x9C3522EDu,
    0x8C96077Bu, 0xB8C96077u, 0x7B8C9607u, 0x77B8C960u,
    0x077B8C96u, 0x6077B8C9u, 0x96077B8Cu, 0xC96077B8u,
};

#define SPC   20                /* samples per chip at 40 MS/s (2 Mchip/s) */
#define PULSE (2 * SPC)         /* half-sine pulse spans two chip periods */

/* Build the O-QPSK waveform for a PHY frame: preamble (4x 0x00), SFD (0xA7), PHR (len), PSDU.
 * out holds IQ words (rf_sample_word) at 40 MS/s. Returns the sample count, 0 if it won't fit.
 * Even chips -> I, odd chips -> Q; each chip is a half-sine of length 2*Tc, Q offset by Tc.
 * Pulses abut on each rail (no overlap), so each output sample comes from exactly one pulse. */
uint32_t rf_154_build(const uint8_t *psdu, unsigned len, uint32_t *out, uint32_t max)
{
    uint8_t sym[2 * (4 + 1 + 1 + 255)];           /* symbols, two per byte (low nibble first) */
    unsigned ns = 0;
    #define PUT(b) do { sym[ns++] = (b) & 0xF; sym[ns++] = (b) >> 4; } while (0)
    for (int i = 0; i < 4; i++) PUT(0x00);
    PUT(0xA7); PUT((uint8_t)len);
    for (unsigned i = 0; i < len; i++) PUT(psdu[i]);
    #undef PUT

    unsigned nchip = ns * 32;
    unsigned nsamp = nchip * SPC + SPC;            /* last Q pulse ends SPC after the last I pulse */
    if (nsamp > max) return 0;

    float pulse[PULSE];
    for (int t = 0; t < PULSE; t++) pulse[t] = 300.0f * sinf((float)M_PI * t / PULSE);
    #define CHIP(cn) ((pn[sym[(cn) >> 5]] >> ((cn) & 31)) & 1)   /* chip cn of the whole stream */

    for (unsigned i = 0; i < nsamp; i++) {
        int I = 0, Q = 0;
        unsigned mI = i / PULSE, cnI = 2 * mI;                    /* I pulse m uses even chip 2m */
        if (cnI < nchip) I = CHIP(cnI) ? (int)pulse[i % PULSE] : -(int)pulse[i % PULSE];
        if (i >= SPC) {
            unsigned mQ = (i - SPC) / PULSE, cnQ = 2 * mQ + 1;    /* Q pulse m uses odd chip 2m+1, offset Tc */
            if (cnQ < nchip) Q = CHIP(cnQ) ? (int)pulse[(i - SPC) % PULSE] : -(int)pulse[(i - SPC) % PULSE];
        }
        out[i] = rf_sample_word(I, Q);
    }
    #undef CHIP
    return nsamp;
}
