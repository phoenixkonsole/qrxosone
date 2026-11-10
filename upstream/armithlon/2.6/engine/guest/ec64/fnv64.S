| FNV-1a over a buffer, in 68100EC64 long mode, assembled by a stock m68k-elf-as: the W prefix
| is a plain word in front of an ordinary .L instruction. Build with build.sh in this folder.
| In: a0 buffer, d1 length. Out: d0 hash. Calls a subroutine per byte, so the stack is in use.
    .macro Q
    .word 0xA008
    .endm
    .text
    .globl _start
_start:
    | d0 = FNV offset basis, d2 = FNV prime, both 64-bit immediates
    .word 0xA008, 0x203C
    .quad 0xCBF29CE484222325
    .word 0xA008, 0x243C
    .quad 0x00000100000001B3
loop:
    bsr.w step
    Q
    subq.l #1,%d1
    bne.w loop
    illegal
step:
    Q
    movem.l %d3,-(%sp)
    moveq #0,%d3
    move.b (%a0)+,%d3
    Q
    eor.l %d3,%d0
    Q
    mulu.l %d2,%d0
    Q
    movem.l (%sp)+,%d3
    rts
