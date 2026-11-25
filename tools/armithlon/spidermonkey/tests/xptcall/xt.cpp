// Standalone test of the m68k64 xptcall mechanics: invoke through an 8-byte slot array, and an asm
// stub that hands its stack arguments to a C++ dispatcher.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef uint64_t u64;
struct Obj {
  virtual unsigned m0(int8_t a, uint16_t b, int32_t c, int64_t d, float e, double f, bool g, void *h, uint32_t i);
};
unsigned Obj::m0(int8_t a, uint16_t b, int32_t c, int64_t d, float e, double f, bool g, void *h, uint32_t i) {
  printf("m0: %d %u %d %lld %g %g %d %p %u\n", a, b, c, (long long)d, e, f, g, h, i);
  return (a == -5 && b == 60000 && c == -123456 && d == -9000000000LL && e == 1.5f && f == -2.25 && g && h == (void *)0x1234 && i == 4000000000u) ? 0 : 1;
}
typedef unsigned (*F)(u64,u64,u64,u64,u64,u64,u64,u64,u64,u64,u64,u64,u64,u64,u64,u64,u64);
static u64 fbits(float x) { uint32_t b; memcpy(&b, &x, 4); return b; }
static u64 dbits(double x) { u64 b; memcpy(&b, &x, 8); return b; }
extern "C" unsigned PrepareAndDispatch(void *self, u64 idx, u64 *args) {
  int8_t a = (int8_t)args[0]; uint16_t b = (uint16_t)args[1]; int32_t c = (int32_t)args[2]; int64_t d = (int64_t)args[3];
  uint32_t eb = (uint32_t)args[4]; float e; memcpy(&e, &eb, 4); double f; memcpy(&f, &args[5], 8);
  bool g = (uint8_t)args[6] != 0; void *h = (void *)args[7]; uint32_t i = (uint32_t)args[8];
  printf("stub %llu self %p: %d %u %d %lld %g %g %d %p %u\n", (unsigned long long)idx, self, a, b, c, (long long)d, e, f, g, h, i);
  return (idx == 3 && a == -5 && b == 60000 && c == -123456 && d == -9000000000LL && e == 1.5f && f == -2.25 && g && h == (void *)0x1234 && i == 4000000000u) ? 0 : 1;
}
asm(".text\n\t.globl stub3\n\t.type stub3,@function\nstub3:\n\t"
    "move.q #3, %d0\n\t"
    "jmp (SharedStub,%pc)\n\t"
    ".type SharedStub,@function\nSharedStub:\n\t"
    "link.w %a6, #-24\n\t"
    "move.q (16,%a6), %d1\n\t"
    "move.q %d1, (%sp)\n\t"
    "move.q %d0, (8,%sp)\n\t"
    "lea (24,%a6), %a0\n\t"
    "move.q %a0, (16,%sp)\n\t"
    "jsr (PrepareAndDispatch,%pc)\n\t"
    "unlk %a6\n\t"
    "rts\n\t");
extern "C" unsigned stub3(void *self, int8_t a, uint16_t b, int32_t c, int64_t d, float e, double f, bool g, void *h, uint32_t i);
int main() {
  Obj o;
  void **vt = *(void ***)&o;
  u64 s[17] = {0};
  s[0] = (u64)&o; s[1] = (u64)(int64_t)(int8_t)-5; s[2] = 60000; s[3] = (u64)(int64_t)-123456; s[4] = (u64)-9000000000LL;
  s[5] = fbits(1.5f); s[6] = dbits(-2.25); s[7] = 1; s[8] = 0x1234; s[9] = 4000000000u;
  unsigned r1 = ((F)vt[0])(s[0],s[1],s[2],s[3],s[4],s[5],s[6],s[7],s[8],s[9],s[10],s[11],s[12],s[13],s[14],s[15],s[16]);
  unsigned r2 = stub3(&o, -5, 60000, -123456, -9000000000LL, 1.5f, -2.25, true, (void *)0x1234, 4000000000u);
  printf("%s\n", r1 == 0 && r2 == 0 ? "XPT PASS" : "XPT FAIL");
  return 0;
}
