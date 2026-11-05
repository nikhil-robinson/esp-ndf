"""Host check for the P4 PIE (Xespv2p2) kernels: unicorn runs the RV32 code, every esp.* instruction (unknown to unicorn)
traps and is emulated here from the TRM "Operation" text (chapter 4.7.2). Instruction text comes from objdump of the same
ELF, so the real assembler decides what each kernel contains. Force-alignment mode (the reset default): loads/stores clear
the low address bits. Only the instructions the ndf kernels use are modelled; an unmodelled one aborts the run."""
import re, struct, subprocess, sys
from unicorn import *
from unicorn.riscv_const import *

M128 = (1 << 128) - 1
ABI = "zero ra sp gp tp t0 t1 t2 s0 s1 a0 a1 a2 a3 a4 a5 a6 a7 s2 s3 s4 s5 s6 s7 s8 s9 s10 s11 t3 t4 t5 t6".split()
ABI += ["fp"]
RET = 0x1000
CODE, DATA, STACK = 0x40000000, 0x20000000, 0x3FF00000


def s(v, bits): return v - (1 << bits) if v >> (bits - 1) & 1 else v
def sat(v, lo, hi): return max(lo, min(hi, v))


class Sim:
    def __init__(self, elf, objdump, nm_tc=None):
        self.uc = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
        self.uc.mem_map(CODE, 0x800000); self.uc.mem_map(DATA, 0x400000); self.uc.mem_map(STACK, 0x100000)
        self.uc.mem_map(RET, 0x1000)
        self.uc.mem_write(RET, b"\x67\x80\x00\x00")  # jalr x0,0(ra) at RET loops; we stop at RET by hook
        d = subprocess.run([objdump, "-d", "--no-show-raw-insn", elf], capture_output=True, text=True, check=True).stdout
        self.ins, self.sym = {}, {}
        for ln in d.splitlines():
            m = re.match(r"\s*([0-9a-f]+):\s+(\S+)\s*(.*)", ln)
            if m: self.ins[int(m[1], 16)] = (m[2], [x.strip() for x in re.split(r"[,\s]+", m[3].split("#")[0].strip()) if x])
            m = re.match(r"([0-9a-f]+) <(\w+)>:", ln)
            if m: self.sym[m[2]] = int(m[1], 16)
        objcopy = objdump.replace("objdump", "objcopy")                    # all loadable segments, flat, at the link address
        flat = elf + ".bin"
        subprocess.run([objcopy, "-O", "binary", elf, flat], check=True)
        self.uc.mem_write(CODE, open(flat, "rb").read())
        self.q = [0] * 8; self.xacc = 0; self.qacc = 0; self.sar = 0; self.sar_byte = 0; self.n = 0
        for pc, (op, _) in self.ins.items():                # PIE words become NOPs; the code hook emulates them first
            if op.startswith("esp."): self.uc.mem_write(pc, b"\x13\x00\x00\x00")
        self.uc.hook_add(UC_HOOK_CODE, self._code, begin=CODE, end=CODE + 0xFFFFF)
        self.dp = DATA + 0x1000

    # memory helpers
    def alloc(self, data, align=16, pad=64):
        self.dp = (self.dp + align - 1) & ~(align - 1)
        a = self.dp; self.uc.mem_write(a, bytes(data)); self.dp += len(data) + pad
        return a
    def rd(self, a, n): return bytes(self.uc.mem_read(a, n))
    def reg(self, n): return self.uc.reg_read(UC_RISCV_REG_X0 + ABI.index(n))
    def setreg(self, n, v): self.uc.reg_write(UC_RISCV_REG_X0 + ABI.index(n), v & 0xFFFFFFFF)

    def call(self, name, *args):
        self.uc.reg_write(UC_RISCV_REG_X0 + 2, STACK + 0x80000)
        for i, a in enumerate(args): self.setreg("a%d" % i, a)
        self.setreg("ra", RET)
        self.uc.emu_start(self.sym[name], RET, count=5_000_000)
        return self.reg("a0")

    def _code(self, uc, pc, size, _):
        op, a = self.ins.get(pc, ("", []))
        if not op.startswith("esp."): return
        r = getattr(self, "i_" + op.replace(".", "_"), None)
        if r is None: raise SystemExit("unmodelled PIE instruction %s %s at %x" % (op, a, pc))
        r(*a); self.n += 1

    # operand helpers
    def Q(self, x): return int(x[1:])
    def g(self, x): return self.reg(x)
    def ld(self, a): return int.from_bytes(self.rd(a & ~15, 16), "little")
    def st(self, a, v): self.uc.mem_write(a & ~15, (v & M128).to_bytes(16, "little"))
    def lanes(self, v, w, signed): 
        n = 128 // w; m = (1 << w) - 1
        return [s(v >> (i * w) & m, w) if signed else v >> (i * w) & m for i in range(n)]
    def pack(self, l, w): return sum((x & ((1 << w) - 1)) << (i * w) for i, x in enumerate(l))
    def inc(self, rs, imm): self.setreg(rs, self.reg(rs) + int(imm, 0))

    # --- loads / stores
    def i_esp_vld_128_ip(self, qu, rs, imm): self.q[self.Q(qu)] = self.ld(self.g(rs)); self.inc(rs, imm)
    def i_esp_vst_128_ip(self, qu, rs, imm): self.st(self.g(rs), self.q[self.Q(qu)]); self.inc(rs, imm)
    def i_esp_ld_128_usar_ip(self, qu, rs, imm):
        self.q[self.Q(qu)] = self.ld(self.g(rs)); self.sar_byte = self.g(rs) & 15; self.inc(rs, imm)
    def i_esp_src_q(self, qa, qx, qy):                      # (qy:qx) >> 8*SAR_BYTE, low 128 bits
        v = (self.q[self.Q(qy)] << 128 | self.q[self.Q(qx)]) >> (8 * self.sar_byte)
        self.q[self.Q(qa)] = v & M128
    def i_esp_orq(self, qa, qx, qy): self.q[self.Q(qa)] = self.q[self.Q(qx)] | self.q[self.Q(qy)]

    # --- accumulators
    def i_esp_zero_xacc(self): self.xacc = 0
    def i_esp_movx_r_xacc_l(self, rd): self.setreg(rd, self.xacc & 0xFFFFFFFF)
    def i_esp_movx_r_xacc_h(self, rd): self.setreg(rd, self.xacc >> 32 & 0xFF)
    def _mac(self, qx, qy, w, signed):
        a, b = self.lanes(self.q[self.Q(qx)], w, signed), self.lanes(self.q[self.Q(qy)], w, signed)
        self.xacc = sat(self.xacc + sum(x * y for x, y in zip(a, b)), -(1 << 39), (1 << 39) - 1)
    def i_esp_vmulas_s16_xacc(self, qx, qy, *_): self._mac(qx, qy, 16, True)
    def i_esp_vmulas_s8_xacc(self, qx, qy, *_): self._mac(qx, qy, 8, True)
    def i_esp_vmulas_s16_xacc_ld_ip(self, qu, rs, imm, qx, qy, *_):
        self._mac(qx, qy, 16, True); self.q[self.Q(qu)] = self.ld(self.g(rs)); self.inc(rs, imm)
    def i_esp_vmulas_s8_xacc_ld_ip(self, qu, rs, imm, qx, qy, *_):
        self._mac(qx, qy, 8, True); self.q[self.Q(qu)] = self.ld(self.g(rs)); self.inc(rs, imm)

    def i_esp_srs_s_xacc(self, rd, rs, *_):
        v = self.xacc >> (self.reg(rs) & 63); self.xacc = v
        self.setreg(rd, sat(v, -(1 << 31), (1 << 31) - 1))
    def i_esp_movx_w_sar(self, rs): self.sar = self.reg(rs) & 63
    def i_esp_vldbc_16_ip(self, qu, rs, imm):
        self.q[self.Q(qu)] = self.pack([int.from_bytes(self.rd(self.g(rs), 2), "little")] * 8, 16); self.inc(rs, imm)
    def i_esp_vld_128_xp(self, qu, rs, rs2): self.q[self.Q(qu)] = self.ld(self.g(rs)); self.setreg(rs, self.reg(rs) + self.reg(rs2))
    def i_esp_zero_qacc(self): self.qacc = 0
    def _qa(self, w): return [s(self.qacc >> (i * w) & ((1 << w) - 1), w) for i in range(512 // w)]
    def _qs(self, l, w): self.qacc = sum((v & ((1 << w) - 1)) << (i * w) for i, v in enumerate(l))
    def i_esp_vmulas_s16_qacc(self, qx, qy, *_):
        a, b = self.lanes(self.q[self.Q(qx)], 16, True), self.lanes(self.q[self.Q(qy)], 16, True)
        self._qs([sat(c + x * y, -(1 << 63), (1 << 63) - 1) for c, x, y in zip(self._qa(64), a, b)], 64)
    def i_esp_vmulas_s8_qacc(self, qx, qy, *_):
        a, b = self.lanes(self.q[self.Q(qx)], 8, True), self.lanes(self.q[self.Q(qy)], 8, True)
        self._qs([sat(c + x * y, -(1 << 31), (1 << 31) - 1) for c, x, y in zip(self._qa(32), a, b)], 32)
    def i_esp_srcmb_s16_qacc(self, qu, rs, mode=None):          # objdump prints ",trunc" for sat = 0, nothing for sat = 1
        sh = self.reg(rs) & 63; l = [c >> sh for c in self._qa(64)]; self._qs(l, 64)
        self.q[self.Q(qu)] = self.pack([sat(c, -32768, 32767) if mode != "trunc" else c for c in l], 16)
    def _qst(self, rs, imm, lo):                                 # 128 bits of QACC (bit offset lo) to memory, pointer += imm
        self.uc.mem_write(self.g(rs), (self.qacc >> lo & M128).to_bytes(16, "little")); self.inc(rs, imm)
    def i_esp_st_qacc_l_l_128_ip(self, rs, imm): self._qst(rs, imm, 0)
    def i_esp_st_qacc_l_h_128_ip(self, rs, imm): self._qst(rs, imm, 128)
    def i_esp_st_qacc_h_l_128_ip(self, rs, imm): self._qst(rs, imm, 256)
    def i_esp_st_qacc_h_h_128_ip(self, rs, imm): self._qst(rs, imm, 384)
    def i_esp_xorq(self, qa, qx, qy): self.q[self.Q(qa)] = self.q[self.Q(qx)] ^ self.q[self.Q(qy)]
    def i_esp_vldbc_8_ip(self, qu, rs, imm):
        self.q[self.Q(qu)] = self.pack([self.rd(self.g(rs), 1)[0]] * 16, 8); self.inc(rs, imm)
    def _mm(self, qz, qx, qy, f):
        a, b = self.lanes(self.q[self.Q(qx)], 8, True), self.lanes(self.q[self.Q(qy)], 8, True)
        self.q[self.Q(qz)] = self.pack([f(x, y) for x, y in zip(a, b)], 8)
    def i_esp_vmax_s8(self, qz, qx, qy): self._mm(qz, qx, qy, max)
    def i_esp_vmin_s8(self, qz, qx, qy): self._mm(qz, qx, qy, min)
    def i_esp_vmul_s16(self, qz, qx, qy, *_):
        a, b = self.lanes(self.q[self.Q(qx)], 16, True), self.lanes(self.q[self.Q(qy)], 16, True)
        self.q[self.Q(qz)] = self.pack([(x * y) >> self.sar for x, y in zip(a, b)], 16)

    def i_esp_vldbc_32_ip(self, qu, rs, imm):
        self.q[self.Q(qu)] = self.pack([int.from_bytes(self.rd(self.g(rs), 4), "little")] * 4, 32); self.inc(rs, imm)
    def i_esp_cmul_s16(self, qz, qx, qy, sel, *_):
        x, y, sel = self.lanes(self.q[self.Q(qx)], 16, True), self.lanes(self.q[self.Q(qy)], 16, True), int(sel)
        z = self.lanes(self.q[self.Q(qz)], 16, True); o = 4 * (sel & 1)
        for k in (0, 2):
            xr, xi, yr, yi = x[o + k], x[o + k + 1], y[o + k], y[o + k + 1]
            if sel < 2: re, im = xr * yr - xi * yi, xr * yi + xi * yr
            else: re, im = xr * yr + xi * yi, xr * yi - xi * yr
            z[o + k], z[o + k + 1] = sat(re >> self.sar, -32768, 32767), sat(im >> self.sar, -32768, 32767)
        self.q[self.Q(qz)] = self.pack(z, 16)
    def i_esp_vsub_s16(self, qv, qx, qy, *_):
        a, b = self.lanes(self.q[self.Q(qx)], 16, True), self.lanes(self.q[self.Q(qy)], 16, True)
        self.q[self.Q(qv)] = self.pack([sat(x - y, -32768, 32767) for x, y in zip(a, b)], 16)

    # --- element-wise
    def i_esp_vadd_s16(self, qv, qx, qy, *_):
        a, b = self.lanes(self.q[self.Q(qx)], 16, True), self.lanes(self.q[self.Q(qy)], 16, True)
        self.q[self.Q(qv)] = self.pack([sat(x + y, -32768, 32767) for x, y in zip(a, b)], 16)
