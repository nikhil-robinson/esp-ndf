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
