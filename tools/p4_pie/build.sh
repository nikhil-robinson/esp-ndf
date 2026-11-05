#!/bin/bash
# build.sh OUT.elf SRC...: assemble / compile the kernels for the host check (RV32 + PIE, no libc), linked at the simulator's code base
set -e
TC=$(ls -d ~/.espressif-v6.1/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin | head -1)
OUT=$1; shift
$TC/riscv32-esp-elf-gcc -march=rv32imac_zicsr_zifencei_xespv -mabi=ilp32 -mno-relax -O2 -nostdlib -ffreestanding -I"$(dirname "$0")/../../components/include" \
    -I"$(dirname "$0")/../../components/soc/esp32p4" -DSOC_CPU_RISCV=1 -Wl,-Ttext=0x40000000 -Wl,--no-warn-rwx-segments -o "$OUT" "$@"
echo "$TC/riscv32-esp-elf-objdump" > "$OUT.objdump"
