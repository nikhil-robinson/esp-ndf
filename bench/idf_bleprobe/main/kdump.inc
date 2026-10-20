/* dump for the advertising diff: EM areas + core + modem; EMRD(off) must read 16-bit EM */
static void adv_kdump(void)
{
    static const uint16_t er[][2] = { { 0x0000, 0x0100 }, { 0x0100, 0x0150 }, { 0x0400, 0x045A }, { 0x1400, 0x1420 }, { 0x2400, 0x2420 } };
    PR("== akstate\n");
    for (unsigned r = 0; r < sizeof er / sizeof er[0]; r++)
        for (uint32_t o = er[r][0]; o < er[r][1]; o += 2) PR("m %04x %04x\n", (unsigned)o, (unsigned)EMRD(o));
    for (uint32_t a = 0x60031000; a < 0x60031400; a += 4) PR("r %08x %08x\n", (unsigned)a, (unsigned)*(volatile uint32_t *)a);
    for (uint32_t a = 0x60011000; a < 0x60011900; a += 4) PR("r %08x %08x\n", (unsigned)a, (unsigned)*(volatile uint32_t *)a);
    for (uint32_t a = 0x60035000; a < 0x60035400; a += 4) PR("r %08x %08x\n", (unsigned)a, (unsigned)*(volatile uint32_t *)a);
    for (uint32_t a = 0x6000E000; a < 0x6000E200; a += 4) PR("r %08x %08x\n", (unsigned)a, (unsigned)*(volatile uint32_t *)a);
    PR("== akstate end\n");
}
