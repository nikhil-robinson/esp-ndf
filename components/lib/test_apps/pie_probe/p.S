#if defined(NDF_P4_REV) && NDF_P4_REV < 3
    .option arch, +xespv2p1
#else
    .option arch, +xespv
#endif
    .text
    .global pie_probe
pie_probe:
    esp.zero.q q2
    esp.vld.128.ip q0, a0, 0
    esp.vld.128.ip q1, a0, 0
    esp.vadd.s16 q3, q0, q1
    esp.vst.128.ip q3, a1, 0
    ret

/* void pie_probe2(const uint32_t *in(8 words), uint32_t *out(16 words)) */
    .global pie_probe2
pie_probe2:
    esp.vld.128.ip q0, a0, 16
    esp.vld.128.ip q1, a0, -16
    esp.vunzip.32 q0, q1
    esp.vst.128.ip q0, a1, 16
    esp.vst.128.ip q1, a1, 16
    esp.vld.128.ip q0, a0, 16
    esp.vld.128.ip q1, a0, -16
    esp.vzip.32 q0, q1
    esp.vst.128.ip q0, a1, 16
    esp.vst.128.ip q1, a1, 16
    ret

/* void pie_probe3(const uint32_t *in(8 words), uint32_t *out(4 words)): 64-bit halves from in[0..1] and in[4..5] */
    .global pie_probe3
pie_probe3:
    esp.zero.q q0
    esp.vld.l.64.ip q0, a0, 16
    esp.vld.h.64.ip q0, a0, 0
    esp.vst.128.ip q0, a1, 0
    ret
