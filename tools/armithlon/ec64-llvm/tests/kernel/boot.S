; Start and exception entry for the test kernel. Privileged instructions the assembler does not
; know yet are written as words: MOVEC is $4E7A/$4E7B, RTE $4E73, STOP $4E72.
    .section .boot, "ax"
    .globl _start
_start:
; Reset: 32-bit mode, running below 4 GB. Into long mode, then to the real code above 4 GB.
    moveq #1, %d0
    .short 0x4E7B, 0x080F  ; movec d0,$80F
    move.q #boot_stack_top, %sp
    move.q #kernel_main, %a0
    jmp (%a0)

    .text
    .macro STUB n
    .globl vec_\n
vec_\n:
    move.q #\n, -(%sp)
    bra exception_common
    .endm

    STUB 2
    STUB 4
    STUB 5
    STUB 8
    STUB 10
    STUB 11
    STUB 29
    STUB 30
    STUB 32
    STUB 33
; The stack now holds the vector number on top of the hardware frame. All registers go
; below it, then USP. See struct regs in kernel.c. The C code returns the stack to go on with,
; in A0 as every pointer.
exception_common:
    suba.q #120, %sp
    movem.q %d0-%d7/%a0-%a6, (0,%sp)
    .short 0x4E7A, 0x0800  ; movec $800,d0
    move.q %d0, -(%sp)
    move.q %sp, %d0
    move.q %d0, -(%sp)
    jsr (exception,%pc)
    adda.q #8, %sp
    move.q %a0, %sp                 ; a pointer comes back in A0
    move.q (%sp)+, %d0
    .short 0x4E7B, 0x0800  ; movec d0,$800
    movem.q (0,%sp), %d0-%d7/%a0-%a6
    adda.q #128, %sp  ; registers and the vector number
    .short 0x4E73  ; rte
; Calls from C: arguments in 8-byte slots above the return address.
    .globl core_id
core_id:
    .short 0x4E7A, 0x080E
    rts
    .globl core_count
core_count:
    .short 0x4E7A, 0x080B
    rts
    .globl set_vbr
set_vbr:
    move.q (8,%sp), %d0
    .short 0x4E7B, 0x0801
    rts
    .globl set_timer
set_timer:
    move.q (8,%sp), %d0
    .short 0x4E7B, 0x080C
    rts
    .globl signal_cores
signal_cores:
    move.q (8,%sp), %d0
    .short 0x4E7B, 0x080D
    rts
    .globl set_start_stack
set_start_stack:
    move.q (8,%sp), %d0
    .short 0x4E7B, 0x080A
    rts
    .globl putc_raw
putc_raw:
    move.q (8,%sp), %d0
    .short 0x4E7B, 0x08F0
    rts
    .globl halt
halt:
    move.q (8,%sp), %d0
    .short 0x4E7B, 0x08F1
    bra halt
    .globl irq_on
irq_on:
    .short 0x46FC, 0x2000  ; move #$2000,sr
    rts
    .globl irq_off
irq_off:
    .short 0x46FC, 0x2700
    rts
    .globl idle_until_irq
idle_until_irq:
    .short 0x4E72, 0x2000  ; stop #$2000
    rts
; Starts a task: its first frame is built by hand, see new_task. resume(sp).
    .globl resume
resume:
    move.q (8,%sp), %d0
    move.q %d0, %sp
    move.q (%sp)+, %d0
    .short 0x4E7B, 0x0800
    movem.q (0,%sp), %d0-%d7/%a0-%a6
    adda.q #128, %sp
    .short 0x4E73
; User state code: one thing it may not do, and one place it may not read.
    .globl user_privileged
user_privileged:
    .short 0x4E72, 0x2000
    rts
    .globl sys_call
sys_call:
    move.q (8,%sp), %d0
    move.q (16,%sp), %d1
    trap #0
    rts
; The first signal to core 1 arrives here. Nothing to return to: drop the frame and go to C.
    .globl secondary_entry
secondary_entry:
    adda.q #16, %sp
    move.q #secondary_c, %a0
    jmp (%a0)
