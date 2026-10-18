# esp-ndf vs ESP-IDF: speed comparison

Every number below was measured on the same ESP32-S3 devkit at 240 MHz, with
the same wiring. The ESP-IDF numbers come from the IDF sibling app
(`bench/idf_periph`, IDF v5.3.5). Cycles are CPU cycles: 240 = 1 µs.
Full details and accuracy checks are in [RESULTS.md](RESULTS.md).

| Peripheral | Operation | esp-ndf | ESP-IDF | Faster by |
|---|---|---|---|---|
| Interrupts | timer match → ISR, level 1 | 76 cycles, 0 jitter | 219–405 | 2.9–5.3× |
| | level 3 | 63 cycles, 0 jitter | 214–274 | 3.4–4.3× |
| | level 5 (fast path) | 46 cycles, 0 jitter | asm-only in IDF | — |
| GPIO | set + clear | 10 cycles | 95 | 9.5× |
| | pin edge → ISR | 191 cycles | 356–393 | 1.9–2.1× |
| | dedicated GPIO set + clear | 3 cycles | 45 | 15× |
| RTC GPIO | set level | 21 cycles | 301 | 14× |
| | get level | 98 cycles | 333 | 3.4× |
| GPTimer | read counter | 110 cycles | 407 | 3.7× |
| | alarm → callback | 800 ns, fixed | 2825–6750 ns | 3.5–8.4× |
| | 10 kHz period jitter | 12 cycles | 828 cycles | 69× less |
| LEDC | duty update | 35 cycles | 763 | 22× |
| MCPWM | compare update | 3 cycles | 75 | 25× |
| | period interrupt jitter, 20 kHz | 0 cycles | 21 | jitter-free |
| PCNT | read count | 85 cycles | 331 | 3.9× |
| RMT | start 100-item transmit | 1156 cycles | 7775 | 6.7× |
| SDM | density update | 35 cycles | 318 | 9.1× |
| UART | write 16 bytes | 279 cycles | 2464 | 8.8× |
| I2C | 16-byte write @100 kHz | 1551 µs (≈ wire time) | 1785 µs | 1.15× (bus-bound) |
| SPI master | 4-byte transaction | 724 cycles | 4756 | 6.6× |
| | 64 B @40 MHz | 5844 cycles | 8407 | 1.4× |
| GDMA | 32 KB memory copy | 63 MB/s | 43 MB/s | 1.5× |
| ADC | one-shot read | 1846 cycles | 6736 | 3.6× |
| TWAI (CAN) | send 8 B @1 Mbit/s incl. wire | 30330 cycles | 33108 | 1.1× (bus-bound) |
| Touch | read | 100 cycles | 529 | 5.3× |
| SPI flash | read 4 KB | 269 µs | 462 µs | 1.7× |
| | write 4 KB | 10.8 ms | 12.4 ms | 1.15× |
| | erase 4 KB | 642 µs | 1041 µs | 1.6× |
| NVS | get u32 | 2338 cycles | 26269 | 11× |
| | update u32 (incl. GC) | 552 µs | 757 µs | 1.4× |
| NVS encrypted | get u32 | 4863 cycles | 82455 | 17× |
| | update u32 | 669 µs | 1542 µs | 2.3× |
| SPIFFS | read 30 KB file | 4 ms | 28 ms | 7× |
| | 40 × 30 KB rewrites (GC) | 7.4 s | 10.9 s | 1.5× |
| FAT + VFS | open file (path lookup) | 65882 cycles | 70187 | 1.07× |

Bus-bound means the wire time dominates and both stacks are close to the
physical limit; esp-ndf just adds less overhead on top.

## Same speed or not comparable

| Peripheral | Why | Accuracy result |
|---|---|---|
| UART throughput | wire-limited | 115200 / 2M / 5M baud: within 0.1 % of wire time on both |
| I2S | IDF's write blocks, ours fills a ring | BCK 1535.949 kHz (IDF 1535.947), 0 under/overruns |
| ADC continuous | no IDF sibling run yet | 50.0000 ms frames, 2-cycle jitter; matches one-shot to 2 LSB |
| LCD | no IDF sibling run | full-screen fill 30.82 ms vs 30.72 ms wire time |
| Wear levelling, FAT format | interop tested, not timed against IDF | volumes interchange both ways |
| Partition table, NVS / FAT host tools | outputs are byte-identical to IDF's | — |
| ADC calibration | same math | bit-identical millivolts to IDF |
