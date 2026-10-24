/* ULP-FSM test program (word addresses): counter in the low 16 bits of word 0x80 (byte 0x200), then HALT; the ULP
 * timer restarts it at word 0 every period */
    .text
    .global entry
entry:
    move    r1, 0x80
    ld      r0, r1, 0
    add     r0, r0, 1
    st      r0, r1, 0
    halt
