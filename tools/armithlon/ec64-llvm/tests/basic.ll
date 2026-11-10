; First functions for the m68k64 target. run-tests.sh runs each in the 68100EC64 reference core
; and compares with what the host computes in run.c.
target triple = "m68k64-unknown-unknown"

define i64 @add64(i64 %a, i64 %b) { %r = add i64 %a, %b
  ret i64 %r }
define i32 @add32(i32 %a, i32 %b) { %r = add i32 %a, %b
  ret i32 %r }
define i64 @logic(i64 %a, i64 %b) {
  %x = xor i64 %a, %b
  %o = or i64 %x, 4080
  %n = and i64 %o, %a
  %s = sub i64 %n, %b
  ret i64 %s }
define i64 @shifts(i64 %a, i64 %n) {
  %l = shl i64 %a, %n
  %r = lshr i64 %a, 7
  %s = ashr i64 %a, %n
  %x = xor i64 %l, %r
  %y = xor i64 %x, %s
  ret i64 %y }
define i64 @muldiv(i64 %a, i64 %b) {
  %m = mul i64 %a, %b
  %d = udiv i64 %m, 7
  %s = sdiv i64 %a, %b
  %r = urem i64 %a, %b
  %t = add i64 %d, %s
  %u = add i64 %t, %r
  ret i64 %u }
define i64 @widen(i32 %a, i16 %b, i8 %c) {
  %x = sext i32 %a to i64
  %y = zext i16 %b to i64
  %z = sext i8 %c to i64
  %t = add i64 %x, %y
  %u = add i64 %t, %z
  ret i64 %u }
define i32 @narrow(i64 %a) {
  %t = trunc i64 %a to i32
  %h = lshr i64 %a, 32
  %u = trunc i64 %h to i32
  %r = sub i32 %t, %u
  ret i32 %r }
define i64 @cmpsel(i64 %a, i64 %b) {
  %c = icmp slt i64 %a, %b
  %s = select i1 %c, i64 %a, i64 %b
  %d = icmp ugt i64 %a, %b
  %e = zext i1 %d to i64
  %r = add i64 %s, %e
  ret i64 %r }
define internal i64 @fib(i64 %n) {
  %c = icmp ult i64 %n, 2
  br i1 %c, label %done, label %more
done:
  ret i64 %n
more:
  %a = sub i64 %n, 1
  %b = sub i64 %n, 2
  %x = call i64 @fib(i64 %a)
  %y = call i64 @fib(i64 %b)
  %r = add i64 %x, %y
  ret i64 %r }
define i64 @callfib(i64 %n) { %r = call i64 @fib(i64 %n)
  ret i64 %r }
define i64 @fnv(ptr %p, i64 %n) {
entry:
  %z = icmp eq i64 %n, 0
  br i1 %z, label %out, label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i2, %loop ]
  %h = phi i64 [ -3750763034362895579, %entry ], [ %h3, %loop ]
  %q = getelementptr i8, ptr %p, i64 %i
  %b = load i8, ptr %q
  %w = zext i8 %b to i64
  %h2 = xor i64 %h, %w
  %h3 = mul i64 %h2, 1099511628211
  %i2 = add i64 %i, 1
  %c = icmp ult i64 %i2, %n
  br i1 %c, label %loop, label %out
out:
  %r = phi i64 [ -3750763034362895579, %entry ], [ %h3, %loop ]
  ret i64 %r }
define i64 @memsum(ptr %p, i64 %n) {
entry:
  %a = alloca [16 x i64]
  br label %fill
fill:
  %i = phi i64 [ 0, %entry ], [ %i2, %fill ]
  %s = getelementptr i64, ptr %a, i64 %i
  %v = mul i64 %i, %n
  store i64 %v, ptr %s
  %t = getelementptr i32, ptr %p, i64 %i
  %w = trunc i64 %v to i32
  store i32 %w, ptr %t
  %i2 = add i64 %i, 1
  %c = icmp ne i64 %i2, 16
  br i1 %c, label %fill, label %sum
sum:
  %j = phi i64 [ 0, %fill ], [ %j2, %sum ]
  %acc = phi i64 [ 0, %fill ], [ %acc2, %sum ]
  %s2 = getelementptr i64, ptr %a, i64 %j
  %v2 = load i64, ptr %s2
  %t2 = getelementptr i32, ptr %p, i64 %j
  %w2 = load i32, ptr %t2
  %w3 = zext i32 %w2 to i64
  %acc1 = add i64 %acc, %v2
  %acc2 = add i64 %acc1, %w3
  %j2 = add i64 %j, 1
  %c2 = icmp ne i64 %j2, 16
  br i1 %c2, label %sum, label %end
end:
  ret i64 %acc2 }
