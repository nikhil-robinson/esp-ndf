/* 4.4: correlator trigger. The capture engine (rf_rx_capture_trigger) fires on energy level;
 * this adds a trigger on a known sync pattern by sliding-correlating a captured IQ window against
 * a reference complex waveform. A full correlator at 16 MS/s exceeds the CPU budget in the stream
 * callback, so the correlation runs over a captured window: it finds and aligns the sync (the
 * trigger point) and reports the peak-to-floor ratio, which is the detection confidence. */
#include "rf.h"

/* Sliding correlation of captured IQ (rf_sample words) against a reference (refI/refQ, rlen taps).
 * Returns the offset of the correlation peak in *peak_off and the peak magnitude in *peak_mag;
 * the function value is the peak-to-median ratio x256 (detection confidence). */
int32_t rf_corr_trigger(const uint32_t *iq, uint32_t n, const int8_t *refI, const int8_t *refQ,
                        uint32_t rlen, uint32_t *peak_off, int64_t *peak_mag)
{
    if (n <= rlen || rlen == 0) return 0;
    uint32_t steps = n - rlen, best_off = 0;
    int64_t best = 0;
    /* magnitude^2 of the complex correlation at each offset; track the peak and a running sum for the floor */
    int64_t sum = 0, cnt = 0;
    for (uint32_t o = 0; o < steps; o++) {
        int64_t re = 0, im = 0;
        for (uint32_t k = 0; k < rlen; k++) {
            uint32_t w = iq[o + k];
            int i = rf_sample_i(w), q = rf_sample_q(w);
            re += (int64_t)i * refI[k] + (int64_t)q * refQ[k];
            im += (int64_t)q * refI[k] - (int64_t)i * refQ[k];
        }
        int64_t mag = re * re + im * im;
        if (mag > best) { best = mag; best_off = o; }
        sum += mag; cnt++;
    }
    *peak_off = best_off; *peak_mag = best;
    int64_t mean = cnt ? sum / cnt : 1;
    if (mean < 1) mean = 1;
    return (int32_t)((best * 256) / mean);           /* peak-to-mean power ratio x256 */
}
