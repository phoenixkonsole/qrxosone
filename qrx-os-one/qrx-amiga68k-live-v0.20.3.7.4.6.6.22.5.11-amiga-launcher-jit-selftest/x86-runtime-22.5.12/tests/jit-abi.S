/* Verify every SysV/AROS m68k C callee-saved register across a generated block. */
.text
.globl qrx_test_callee_saved
.type qrx_test_callee_saved,@function
qrx_test_callee_saved:
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    movea.l 48(%sp),%a1
    movea.l 52(%sp),%a0
    move.l %a0,-(%sp)
    move.l #0x22334455,%d2
    move.l %d2,%d3
    move.l %d2,%d4
    move.l %d2,%d5
    move.l %d2,%d6
    move.l %d2,%d7
    movea.l %d2,%a2
    movea.l %d2,%a3
    movea.l %d2,%a4
    movea.l %d2,%a5
    movea.l %d2,%a6
    jsr (%a1)
    addq.l #4,%sp
    cmp.l #0x22334455,%d2
    bne 1f
    cmp.l %d2,%d3
    bne 1f
    cmp.l %d2,%d4
    bne 1f
    cmp.l %d2,%d5
    bne 1f
    cmp.l %d2,%d6
    bne 1f
    cmp.l %d2,%d7
    bne 1f
    cmpa.l %d2,%a2
    bne 1f
    cmpa.l %d2,%a3
    bne 1f
    cmpa.l %d2,%a4
    bne 1f
    cmpa.l %d2,%a5
    bne 1f
    cmpa.l %d2,%a6
    bne 1f
    moveq #1,%d0
    bra 2f
1:  moveq #0,%d0
2:  movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts
.section .note.GNU-stack,"",@progbits
