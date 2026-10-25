"""Merge components/lib/test_apps/dsp_lib (ndf_result.txt) and bench/idf_dsplib (idf_result.txt) cycle counts into a markdown table."""
import re, os
d = os.path.dirname(os.path.abspath(__file__))
def load(f, pre):
    r = {}
    for l in open(os.path.join(d, f)):
        m = re.match(pre + r" (.+?)\s+(\d+) cycles", l)
        if m: r[m.group(1).strip()] = int(m.group(2))
    return r
ndf, idf = load("ndf_result.txt", "ndf"), load("idf_result.txt", "idf")
pairs = [("fft_f32 256", "fft4r_fc32+bitrev 256"), ("fft_f32 1024", "fft4r_fc32+bitrev 1024"), ("fft_f32 4096", "fft4r_fc32+bitrev 4096"),
         ("rfft_f32 1024", "real fft 1024 (fft2r 512+c2r)"), ("fft_s16 1024", "fft2r_sc16+bitrev 1024"),
         ("win_hann 1024", "wind_hann 1024"), ("win_blackman 1024", "wind_blackman 1024"), ("win_flattop 1024", "wind_flat_top 1024"),
         ("fir_f32 64 taps x 1024", "fir_f32 64 taps x 1024"), ("fird_s16 32 taps /4 x 1024", "fird_s16 32 taps /4 x 1024"),
         ("biquad_f32 x 1024", "biquad_f32 x 1024"), ("add_f32 1024", "add_f32 1024"), ("mul_f32 1024", "mul_f32 1024"),
         ("mulc_f32 1024", "mulc_f32 1024"), ("add_s16 1024", "add_s16 1024"), ("mul_s16 1024 >>15", "mul_s16 1024"),
         ("dot_f32 1024", "dotprod_f32 1024"), ("mat_mul_f32 16x16x16", "mult_f32 16x16x16"), ("mat_mul_f32 64x64x64", "mult_f32 64x64x64"),
         ("mat_mul_s16 64x64x64 >>15", "mult_s16 64x64x64 >>15"), ("conv_f32 1024 * 64", "conv_f32 1024 * 64"),
         ("corr_f32 1024 . 64", "corr_f32 1024 . 64"), ("dct_f32 256", "dct_f32 256")]
print("| cycles | esp-ndf | ESP-DSP 1.8.2 | gain |\n|---|---|---|---|")
for a, b in pairs:
    if a in ndf and b in idf: print(f"| {a} | {ndf[a]} | {idf[b]} | {idf[b] / ndf[a]:.2f}x |")
