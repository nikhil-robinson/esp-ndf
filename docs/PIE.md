# PIE vector unit cookbook (ESP32-S3)

The S3's AI/DSP acceleration is the PIE extension of the LX7 core ("cop_ai", coprocessor 3): eight 128-bit
registers q0-q7, the 40-bit ACCX accumulator, the per-lane QACC accumulator (16 x 20-bit for s8, 8 x 40-bit for s16),
SAR_BYTE / UA_STATE for unaligned access and FFT_BIT_WIDTH. Enabled at boot (CPENABLE bit 3, SOC_CPENABLE_BOOT).

## Measured throughput (`components/arch/test_apps/pie_bench`, BOX-3, 240 MHz, internal RAM, 2026-10-24)

| instruction (x8 per loop iteration, 256 iterations) | cycles each |
|---|---|
| `nop (loop overhead reference)` | 1.00 |
| `ee.vld.128.ip q0 (aligned, +16)` | 1.00 |
| `ee.vld.128.ip q0 streaming 4 KB` | 1.00 |
| `ee.vst.128.ip q0` | 1.00 |
| `ee.ld.128.usar.ip (unaligned load, +1)` | 1.00 |
| `ee.src.q q2,q0,q1 (unaligned combine)` | 1.00 |
| `ee.vmulas.s16.accx q0,q1 (independent)` | 1.00 |
| `ee.vmulas.s8.accx q0,q1` | 1.00 |
| `ee.vmulas.s16.qacc q0,q1 (per lane)` | 1.00 |
| `ee.vmulas.s8.qacc q0,q1 (per lane)` | 1.00 |
| `ee.vmulas.s16.accx.ld.ip (MAC + load)` | 1.00 |
| `ee.vmulas.s8.qacc.ld.ip (MAC + load)` | 1.00 |
| `ee.vadds.s16 q2,q0,q1 (independent)` | 1.00 |
| `ee.vadds.s16 q0,q0,q1 (dependent chain)` | 1.00 |
| `ee.vmul.s16 q2,q0,q1` | 1.00 |
| `ee.vmul.s16 q0,q0,q1 (dependent)` | 2.00 |
| `ee.vldbc.16.ip (broadcast load)` | 1.00 |
| `ee.zero.qacc` | 1.00 |
| `ee.srs.accx (read ACCX, shift)` | 1.00 |
| `ee.vld.128.ip then dependent vadds (load-use)` | 3.00 |
| `ee.movi.32.a (lane to AR)` | 1.00 |

Rules that follow from the table:
- Every listed instruction issues at 1/cycle when independent, including the fused MAC + load forms
  (`ee.vmulas.*.ld.ip`): one 16-lane s8 or 8-lane s16 MAC plus one 128-bit load per cycle is the kernel floor.
- **Load-use latency is 2 extra cycles**: a q register loaded by `ee.vld.*` must not be used by the next 2
  instructions (load the next block while multiplying the current one: software-pipeline by one step).
- `ee.vmul` has 1 extra cycle of result latency; adds and MACs into accumulators do not stall.
- Unaligned data: `ee.ld.128.usar.ip` + `ee.src.q` costs nothing extra in throughput (both 1/cycle) - keep two
  loads in flight and combine; aligned 16-byte buffers avoid even that.
- QACC lanes are 20-bit for s8: at most 32 accumulations of full-range int8 products (2^14 each) before a lane can
  overflow; flush to 32-bit (`ee.st.qacc_*`/`ee.srcmb`) at least that often. ACCX is 40-bit (no practical limit).

## Loop buffer quirk

GCC for the S3 emits `wsr.lbeg` with a dummy value (plus a `nop`) right before every zero-overhead `loop`:
the core's loop buffer can keep a stale LBEG otherwise. Hand-written assembly loops should do the same.
Zero-overhead loops only loop back with PS.EXCM = 0: the interrupt entry clears EXCM for that reason
(components/arch/xtensa/vectors.S).

## PIE in interrupt handlers

Allowed at every level, nested (lazy save, like the FPU): handlers start with CP3 off, the first PIE instruction
traps once (EXCCAUSE 35), the interrupted context's 208-byte state (IDF xchal_cp3 layout) goes to a slot per
(core, INTLEVEL) and comes back at handler exit. Cost: 0 for handlers that do not use PIE; a handler that does
(dsp_dot_s16 of 64 + a float op) adds 63 cycles to the round trip at level 3 (135 -> 198, `components/arch/test_apps/pie_isr`).
IDF does not allow PIE in ISRs.
