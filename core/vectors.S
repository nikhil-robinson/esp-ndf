/*
 * ESP32-S3 Interrupt/Exception Vector Table
 * Xtensa LX7 exception handling
 */

    .section .vector_table, "ax"
    .align 256
    .global _vector_table
_vector_table:

/* Level 1 interrupt / Exception (User) - offset 0x00 */
    .org    _vector_table + 0x00
    .global _UserExceptionVector
_UserExceptionVector:
    wsr     a0, excsave1
    call0   _user_exception_handler
    
/* Level 2 interrupt - offset 0x180 */
    .org    _vector_table + 0x180
    .global _Level2Vector
_Level2Vector:
    wsr     a0, excsave2
    call0   _level2_int_handler

/* Level 3 interrupt - offset 0x1C0 */
    .org    _vector_table + 0x1C0
    .global _Level3Vector
_Level3Vector:
    wsr     a0, excsave3
    call0   _level3_int_handler

/* Level 4 interrupt - offset 0x200 */
    .org    _vector_table + 0x200
    .global _Level4Vector
_Level4Vector:
    wsr     a0, excsave4
    call0   _level4_int_handler

/* Level 5 interrupt (Debug) - offset 0x240 */
    .org    _vector_table + 0x240
    .global _Level5Vector
_Level5Vector:
    wsr     a0, excsave5
    call0   _level5_int_handler

/* NMI - offset 0x2C0 */
    .org    _vector_table + 0x2C0
    .global _NMIExceptionVector
_NMIExceptionVector:
    wsr     a0, excsave6
    call0   _nmi_handler

/* Kernel exception - offset 0x300 */
    .org    _vector_table + 0x300
    .global _KernelExceptionVector
_KernelExceptionVector:
    wsr     a0, excsave1
    call0   _kernel_exception_handler

/* Double exception - offset 0x3C0 */
    .org    _vector_table + 0x3C0
    .global _DoubleExceptionVector
_DoubleExceptionVector:
    /* Double exception - fatal, just halt */
    j       _double_exception_handler


/*
 * Default exception/interrupt handlers (weak symbols)
 */
    .section .text
    .align 4

    .weak   _user_exception_handler
    .type   _user_exception_handler, @function
_user_exception_handler:
    /* Read exception cause */
    rsr     a2, exccause
    /* Store exception PC */
    rsr     a3, epc1
    /* Default: halt */
    j       .
    
    .weak   _kernel_exception_handler
    .type   _kernel_exception_handler, @function
_kernel_exception_handler:
    j       .

    .weak   _level2_int_handler
    .type   _level2_int_handler, @function
_level2_int_handler:
    rsr     a0, excsave2
    rfi     2

    .weak   _level3_int_handler
    .type   _level3_int_handler, @function
_level3_int_handler:
    rsr     a0, excsave3
    rfi     3

    .weak   _level4_int_handler
    .type   _level4_int_handler, @function
_level4_int_handler:
    rsr     a0, excsave4
    rfi     4

    .weak   _level5_int_handler
    .type   _level5_int_handler, @function
_level5_int_handler:
    rsr     a0, excsave5
    rfi     5

    .weak   _nmi_handler
    .type   _nmi_handler, @function
_nmi_handler:
    rsr     a0, excsave6
    rfi     6

    .weak   _double_exception_handler
    .type   _double_exception_handler, @function
_double_exception_handler:
    j       .
