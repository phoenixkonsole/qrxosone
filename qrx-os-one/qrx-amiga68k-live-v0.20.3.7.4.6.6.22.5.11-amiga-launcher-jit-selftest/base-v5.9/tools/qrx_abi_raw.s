/*
 * Optional freestanding link probe.
 * This bypasses the normal C runtime if the installed linker accepts it.
 */
        .text
        .globl _start
_start:
        moveq   #0,%d0
        rts
