.text
.global qrx_reference_fast_loop
qrx_reference_fast_loop:
    ldr w9, [x0,#0]
    ldr w10, [x0,#4]
    movz w11,#1
qrx_fast_loop:
    add w9,w9,w11
    subs w10,w10,#1
    b.ne qrx_fast_loop
    str w9,[x0,#8]
    ret
