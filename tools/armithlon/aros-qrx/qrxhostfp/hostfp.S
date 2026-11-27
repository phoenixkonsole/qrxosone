/* written by make-hostfp.py: soft float as the 68100EC64 host floating point instruction */
	.text

	.globl	__addsf3
	.p2align	1
	.type	__addsf3,@function
__addsf3:
	move.l	(12,%sp), %d0
	move.l	(20,%sp), %d1
	.short	0xAF00
	rts
	.size	__addsf3, .-__addsf3

	.globl	__subsf3
	.p2align	1
	.type	__subsf3,@function
__subsf3:
	move.l	(12,%sp), %d0
	move.l	(20,%sp), %d1
	.short	0xAF01
	rts
	.size	__subsf3, .-__subsf3

	.globl	__mulsf3
	.p2align	1
	.type	__mulsf3,@function
__mulsf3:
	move.l	(12,%sp), %d0
	move.l	(20,%sp), %d1
	.short	0xAF02
	rts
	.size	__mulsf3, .-__mulsf3

	.globl	__divsf3
	.p2align	1
	.type	__divsf3,@function
__divsf3:
	move.l	(12,%sp), %d0
	move.l	(20,%sp), %d1
	.short	0xAF03
	rts
	.size	__divsf3, .-__divsf3

	.globl	__lesf2
	.p2align	1
	.type	__lesf2,@function
__lesf2:
	move.l	(12,%sp), %d0
	move.l	(20,%sp), %d1
	.short	0xAF04
	rts
	.size	__lesf2, .-__lesf2

	.globl	__ltsf2
	.p2align	1
	.type	__ltsf2,@function
__ltsf2:
	move.l	(12,%sp), %d0
	move.l	(20,%sp), %d1
	.short	0xAF04
	rts
	.size	__ltsf2, .-__ltsf2

	.globl	__eqsf2
	.p2align	1
	.type	__eqsf2,@function
__eqsf2:
	move.l	(12,%sp), %d0
	move.l	(20,%sp), %d1
	.short	0xAF04
	rts
	.size	__eqsf2, .-__eqsf2

	.globl	__nesf2
	.p2align	1
	.type	__nesf2,@function
__nesf2:
	move.l	(12,%sp), %d0
	move.l	(20,%sp), %d1
	.short	0xAF04
	rts
	.size	__nesf2, .-__nesf2

	.globl	__gesf2
	.p2align	1
	.type	__gesf2,@function
__gesf2:
	move.l	(12,%sp), %d0
	move.l	(20,%sp), %d1
	.short	0xAF05
	rts
	.size	__gesf2, .-__gesf2

	.globl	__gtsf2
	.p2align	1
	.type	__gtsf2,@function
__gtsf2:
	move.l	(12,%sp), %d0
	move.l	(20,%sp), %d1
	.short	0xAF05
	rts
	.size	__gtsf2, .-__gtsf2

	.globl	__unordsf2
	.p2align	1
	.type	__unordsf2,@function
__unordsf2:
	move.l	(12,%sp), %d0
	move.l	(20,%sp), %d1
	.short	0xAF06
	rts
	.size	__unordsf2, .-__unordsf2

	.globl	__fixsfsi
	.p2align	1
	.type	__fixsfsi,@function
__fixsfsi:
	move.l	(12,%sp), %d0
	.short	0xAF07
	rts
	.size	__fixsfsi, .-__fixsfsi

	.globl	__fixunssfsi
	.p2align	1
	.type	__fixunssfsi,@function
__fixunssfsi:
	move.l	(12,%sp), %d0
	.short	0xAF08
	rts
	.size	__fixunssfsi, .-__fixunssfsi

	.globl	__fixsfdi
	.p2align	1
	.type	__fixsfdi,@function
__fixsfdi:
	move.l	(12,%sp), %d0
	.short	0xAF09
	rts
	.size	__fixsfdi, .-__fixsfdi

	.globl	__fixunssfdi
	.p2align	1
	.type	__fixunssfdi,@function
__fixunssfdi:
	move.l	(12,%sp), %d0
	.short	0xAF0A
	rts
	.size	__fixunssfdi, .-__fixunssfdi

	.globl	__floatsisf
	.p2align	1
	.type	__floatsisf,@function
__floatsisf:
	move.l	(12,%sp), %d0
	.short	0xAF0B
	rts
	.size	__floatsisf, .-__floatsisf

	.globl	__floatunsisf
	.p2align	1
	.type	__floatunsisf,@function
__floatunsisf:
	move.l	(12,%sp), %d0
	.short	0xAF0C
	rts
	.size	__floatunsisf, .-__floatunsisf

	.globl	__floatdisf
	.p2align	1
	.type	__floatdisf,@function
__floatdisf:
	move.q	(8,%sp), %d0
	.short	0xAF0D
	rts
	.size	__floatdisf, .-__floatdisf

	.globl	__floatundisf
	.p2align	1
	.type	__floatundisf,@function
__floatundisf:
	move.q	(8,%sp), %d0
	.short	0xAF0E
	rts
	.size	__floatundisf, .-__floatundisf

	.globl	__extendsfdf2
	.p2align	1
	.type	__extendsfdf2,@function
__extendsfdf2:
	move.l	(12,%sp), %d0
	.short	0xAF0F
	rts
	.size	__extendsfdf2, .-__extendsfdf2

	.globl	sqrtf
	.p2align	1
	.type	sqrtf,@function
sqrtf:
	move.l	(12,%sp), %d0
	.short	0xAF10
	rts
	.size	sqrtf, .-sqrtf

	.globl	__adddf3
	.p2align	1
	.type	__adddf3,@function
__adddf3:
	move.q	(8,%sp), %d0
	move.q	(16,%sp), %d1
	.short	0xAF20
	rts
	.size	__adddf3, .-__adddf3

	.globl	__subdf3
	.p2align	1
	.type	__subdf3,@function
__subdf3:
	move.q	(8,%sp), %d0
	move.q	(16,%sp), %d1
	.short	0xAF21
	rts
	.size	__subdf3, .-__subdf3

	.globl	__muldf3
	.p2align	1
	.type	__muldf3,@function
__muldf3:
	move.q	(8,%sp), %d0
	move.q	(16,%sp), %d1
	.short	0xAF22
	rts
	.size	__muldf3, .-__muldf3

	.globl	__divdf3
	.p2align	1
	.type	__divdf3,@function
__divdf3:
	move.q	(8,%sp), %d0
	move.q	(16,%sp), %d1
	.short	0xAF23
	rts
	.size	__divdf3, .-__divdf3

	.globl	__ledf2
	.p2align	1
	.type	__ledf2,@function
__ledf2:
	move.q	(8,%sp), %d0
	move.q	(16,%sp), %d1
	.short	0xAF24
	rts
	.size	__ledf2, .-__ledf2

	.globl	__ltdf2
	.p2align	1
	.type	__ltdf2,@function
__ltdf2:
	move.q	(8,%sp), %d0
	move.q	(16,%sp), %d1
	.short	0xAF24
	rts
	.size	__ltdf2, .-__ltdf2

	.globl	__eqdf2
	.p2align	1
	.type	__eqdf2,@function
__eqdf2:
	move.q	(8,%sp), %d0
	move.q	(16,%sp), %d1
	.short	0xAF24
	rts
	.size	__eqdf2, .-__eqdf2

	.globl	__nedf2
	.p2align	1
	.type	__nedf2,@function
__nedf2:
	move.q	(8,%sp), %d0
	move.q	(16,%sp), %d1
	.short	0xAF24
	rts
	.size	__nedf2, .-__nedf2

	.globl	__gedf2
	.p2align	1
	.type	__gedf2,@function
__gedf2:
	move.q	(8,%sp), %d0
	move.q	(16,%sp), %d1
	.short	0xAF25
	rts
	.size	__gedf2, .-__gedf2

	.globl	__gtdf2
	.p2align	1
	.type	__gtdf2,@function
__gtdf2:
	move.q	(8,%sp), %d0
	move.q	(16,%sp), %d1
	.short	0xAF25
	rts
	.size	__gtdf2, .-__gtdf2

	.globl	__unorddf2
	.p2align	1
	.type	__unorddf2,@function
__unorddf2:
	move.q	(8,%sp), %d0
	move.q	(16,%sp), %d1
	.short	0xAF26
	rts
	.size	__unorddf2, .-__unorddf2

	.globl	__fixdfsi
	.p2align	1
	.type	__fixdfsi,@function
__fixdfsi:
	move.q	(8,%sp), %d0
	.short	0xAF27
	rts
	.size	__fixdfsi, .-__fixdfsi

	.globl	__fixunsdfsi
	.p2align	1
	.type	__fixunsdfsi,@function
__fixunsdfsi:
	move.q	(8,%sp), %d0
	.short	0xAF28
	rts
	.size	__fixunsdfsi, .-__fixunsdfsi

	.globl	__fixdfdi
	.p2align	1
	.type	__fixdfdi,@function
__fixdfdi:
	move.q	(8,%sp), %d0
	.short	0xAF29
	rts
	.size	__fixdfdi, .-__fixdfdi

	.globl	__fixunsdfdi
	.p2align	1
	.type	__fixunsdfdi,@function
__fixunsdfdi:
	move.q	(8,%sp), %d0
	.short	0xAF2A
	rts
	.size	__fixunsdfdi, .-__fixunsdfdi

	.globl	__floatsidf
	.p2align	1
	.type	__floatsidf,@function
__floatsidf:
	move.l	(12,%sp), %d0
	.short	0xAF2B
	rts
	.size	__floatsidf, .-__floatsidf

	.globl	__floatunsidf
	.p2align	1
	.type	__floatunsidf,@function
__floatunsidf:
	move.l	(12,%sp), %d0
	.short	0xAF2C
	rts
	.size	__floatunsidf, .-__floatunsidf

	.globl	__floatdidf
	.p2align	1
	.type	__floatdidf,@function
__floatdidf:
	move.q	(8,%sp), %d0
	.short	0xAF2D
	rts
	.size	__floatdidf, .-__floatdidf

	.globl	__floatundidf
	.p2align	1
	.type	__floatundidf,@function
__floatundidf:
	move.q	(8,%sp), %d0
	.short	0xAF2E
	rts
	.size	__floatundidf, .-__floatundidf

	.globl	__truncdfsf2
	.p2align	1
	.type	__truncdfsf2,@function
__truncdfsf2:
	move.q	(8,%sp), %d0
	.short	0xAF2F
	rts
	.size	__truncdfsf2, .-__truncdfsf2

	.globl	sqrt
	.p2align	1
	.type	sqrt,@function
sqrt:
	move.q	(8,%sp), %d0
	.short	0xAF30
	rts
	.size	sqrt, .-sqrt
