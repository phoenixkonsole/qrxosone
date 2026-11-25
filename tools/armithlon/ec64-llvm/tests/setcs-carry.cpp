// QRX: regression test for the SETCS_C lowering of the m68k64 backend (setcs-carry.sh)
#include <stdio.h>
#include <stdint.h>
struct R { uint32_t v; uint32_t e; };
__attribute__((noinline)) R mk(uint32_t cap, int fail) { R r; if (fail) { r.v = 0; r.e = 0x8007000e; } else { r.v = cap ? cap + 11 : 0; r.e = 0; } return r; }
__attribute__((noinline)) uint32_t sel(uint32_t e, uint32_t v) { return e == 0 ? v : 0xffffffffu; }
__attribute__((noinline)) uint32_t viastruct(uint32_t cap, int fail) { R r = mk(cap, fail); return sel(r.e, r.v); }
__attribute__((noinline)) uint32_t direct(uint32_t cap, int fail) { R r = mk(cap, fail); return r.e == 0 ? r.v : 0xffffffffu; }
int main() {
  volatile int z = 0, one = 1;
  R a = mk(16, z);
  printf("mk(16,0) = v %u e %#x\n", a.v, a.e);
  printf("sel(0,27) = %u, sel(5,27) = %u\n", sel(0, 27), sel(5, 27));
  printf("viastruct(16,0) = %u, (16,1) = %u\n", viastruct(16, z), viastruct(16, one));
  printf("direct(16,0) = %u, (16,1) = %u\n", direct(16, z), direct(16, one));
}
