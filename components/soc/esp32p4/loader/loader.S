/* P4 loader entry: the ROM jumps here with an undefined stack; the loader has its own at the top of its window. */
    .section .text.entry, "ax"
    .global _loader_start
_loader_start:
    la      sp, _loader_stack_top
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    call    loader_main
1:  j       1b
