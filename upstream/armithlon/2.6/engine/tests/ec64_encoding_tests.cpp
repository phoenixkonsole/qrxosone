/* SpiderMonkey's m68k64 encoder (tools/armithlon/spidermonkey/jit-m68k64/Encoding-m68k64.h) against the
 * 68100EC64's reference interpreter: each sequence it encodes runs, and the registers and memory
 * are checked. Covers D8-D15 and A8-A15 (the prefix), .Q, every addressing mode the encoder makes,
 * 32-bit branches with labels, and the FPU. */
extern "C" {
#include "qrx68100ec64.h"
}
#include "../../../../../tools/armithlon/spidermonkey/jit-m68k64/Encoding-m68k64.h"
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace js::jit::m68k64enc;

static const uint64_t RAM = uint64_t(64) << 20, CODE = 0x10000, DATA = 0x200000;
static uint8_t* ram;
static qec64_cpu c;
static unsigned checks, failures;
#define CHECK(x, ...) do { checks++; if (!(x)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); puts(""); failures++; } } while (0)

static uint64_t get(uint64_t a, unsigned n) { uint64_t v = 0; for (unsigned i = 0; i < n; i++) v = v << 8 | ram[a + i]; return v; }
static void set(uint64_t a, unsigned n, uint64_t v) { for (unsigned i = 0; i < n; i++) ram[a + i] = uint8_t(v >> (8 * (n - 1 - i))); }

/* A tiny assembler: instructions go in a row; labels and 32-bit branches patched on bind */
struct Asm {
  std::vector<uint16_t> w;
  bool bad = false;
  void put(const Insn& i) { if (!i.ok) bad = true; for (unsigned k = 0; k < i.n; k++) w.push_back(i.w[k]); }
  size_t here() const { return w.size() * 2; }
  struct Label { long at = -1; std::vector<size_t> uses; };
  void branch(Cond cc, Label& l) {
    size_t ext = here() + 2;
    put(branch32(cc, l.at >= 0 ? int32_t(l.at - long(ext)) : 0));
    if (l.at < 0) l.uses.push_back(ext);
  }
  void fbranch(FCond cc, Label& l) {
    size_t ext = here() + 2;
    put(fbranch32(cc, l.at >= 0 ? int32_t(l.at - long(ext)) : 0));
    if (l.at < 0) l.uses.push_back(ext);
  }
  void bind(Label& l) {
    l.at = long(here());
    for (size_t e : l.uses) { int32_t d = int32_t(l.at - long(e)); w[e / 2] = uint16_t(uint32_t(d) >> 16); w[e / 2 + 1] = uint16_t(d); }
  }
};

static enum qec64_status run(const Asm& a)
{
  for (size_t i = 0; i < a.w.size(); i++) set(CODE + i * 2, 2, a.w[i]);
  set(CODE + a.w.size() * 2, 2, 0x4AFC);    /* ILLEGAL: the core stops */
  c.pc = CODE;
  return qec64_run(&c, 1000000);
}
static void fresh()
{
  qec64_init(&c, ram, RAM, 0);
  c.m64 = true;
  c.a[7] = 0x300000;
}
static uint64_t& R(Reg r) { return isAddr(r) ? c.a[num(r)] : c.d[num(r)]; }

static void moves()
{
  /* constants of each width into every register but the stack pointer and A8 */
  const uint64_t vals[] = { 5, uint64_t(-3), 0x7FFFFFFF, 0xFFFFFFFF, 0x123456789ABCDEF0ull, uint64_t(-100000) };
  for (unsigned r = 0; r < 32; r++) {
    if (r == A(7) || r == A(8)) continue;
    for (uint64_t v : vals) {
      fresh(); Asm a; a.put(moveImm(v, Reg(r)));
      CHECK(!a.bad && run(a) == QEC64_STOPPED && R(Reg(r)) == v, "moveImm %016" PRIx64 " to r%u: %016" PRIx64, v, r, R(Reg(r)));
    }
  }
  /* MOVE of each size between registers and memory, every addressing mode, high registers */
  for (Size s : { Size::B, Size::W, Size::L, Size::Q }) {
    unsigned n = unsigned(s);
    fresh(); Asm a;
    a.put(moveImm(DATA, A(10)));
    a.put(moveImm(0x1122334455667788ull, D(9)));
    a.put(move(s, Ea::reg(D(9)), Ea::disp32(A(10), 16)));          /* (d16,An) */
    a.put(move(s, Ea::disp32(A(10), 16), Ea::reg(D(12))));
    a.put(move(s, Ea::reg(D(9)), Ea::disp32(A(10), 0x12345)));     /* (bd32,An) */
    a.put(moveImm(3, D(11)));
    a.put(move(s, Ea::reg(D(9)), Ea::indexed(A(10), D(11), 3, 100)));   /* (bd,An,Dn*8) */
    a.put(move(s, Ea::indexed(A(10), D(11), 3, 100), Ea::reg(D(13))));
    a.put(move(s, Ea::reg(D(9)), Ea::indexed(A(10), A(10), 0, -0x1000)));   /* index An, beyond 8 bits */
    a.put(move(s, Ea::reg(D(9)), Ea::preDec(A(7))));
    a.put(move(s, Ea::postInc(A(7)), Ea::reg(D(14))));
    CHECK(!a.bad && run(a) == QEC64_STOPPED, "move %u: assembled", n);
    uint64_t m = n == 8 ? ~0ull : (1ull << (8 * n)) - 1, want = 0x1122334455667788ull & m;
    CHECK(get(DATA + 16, n) == want && (R(D(12)) & m) == want, "move.%u (d16,An): %016" PRIx64, n, R(D(12)));
    CHECK(get(DATA + 0x12345, n) == want, "move.%u (bd32,An)", n);
    CHECK(get(DATA + 124, n) == want && (R(D(13)) & m) == want, "move.%u indexed: %016" PRIx64, n, R(D(13)));
    CHECK(get(2 * DATA - 0x1000, n) == want, "move.%u (An,An)", n);
    CHECK((R(D(14)) & m) == want && c.a[7] == 0x300000, "move.%u stack: %016" PRIx64 " sp %" PRIx64, n, R(D(14)), c.a[7]);
  }
  /* LEA, PEA, EXG, PC-relative loads */
  fresh(); { Asm a;
    a.put(moveImm(DATA, A(3)));
    a.put(moveImm(2, D(1)));
    a.put(lea(Ea::indexed(A(3), D(1), 2, 0x40000), A(12)));
    a.put(pea(Ea::disp32(A(3), 8)));
    a.put(move(Size::Q, Ea::postInc(A(7)), Ea::reg(D(5))));
    a.put(moveImm(7, D(10)));
    a.put(exg(D(10), A(12)));
    a.put(move(Size::L, Ea::pcRel(8), Ea::reg(D(6))));    /* from its extension word: 2 more of it and the 6 of the BRA */
    a.put(branch32(CC_T, 8));                            /* over the data: 4 to the end of the BRA, 4 of data */
    a.put(Insn{ { 0xCAFE, 0xBABE }, 2 });
    CHECK(!a.bad && run(a) == QEC64_STOPPED, "lea/pea/exg");
    CHECK(R(D(10)) == DATA + 8 + 0x40000 && R(A(12)) == 7 && R(D(5)) == DATA + 8 && R(D(6)) == 0xCAFEBABE,
          "lea %" PRIx64 " exg %" PRIx64 " pea %" PRIx64 " pcrel %08" PRIx64, R(D(10)), R(A(12)), R(D(5)), R(D(6)));
  }
}

static void arithmetic()
{
  /* each ALU op at each size against the host, high registers on both sides */
  const uint64_t xs[] = { 0, 1, 0x7F, 0x80, 0xFFFF, 0x80000000, 0xFFFFFFFF, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0x0123456789ABCDEFull };
  for (int op = 0; op < 5; op++)
    for (Size s : { Size::B, Size::W, Size::L, Size::Q })
      for (uint64_t x : xs)
        for (uint64_t y : xs) {
          unsigned n = unsigned(s);
          uint64_t m = n == 8 ? ~0ull : (1ull << (8 * n)) - 1, want;
          switch (op) {
            case 0: want = (x + y) & m; break;
            case 1: want = (x - y) & m; break;
            case 2: want = x & y & m; break;
            case 3: want = (x | y) & m; break;
            default: want = (x ^ y) & m; break;
          }
          fresh(); Asm a;
          a.put(moveImm(x, D(13)));
          a.put(moveImm(y, D(2)));
          if (op == 4) a.put(aluToEa(AluOp::Eor, s, D(2), Ea::reg(D(13))));
          else a.put(alu(AluOp(op), s, Ea::reg(D(2)), D(13)));
          /* the same through memory, op Dn,<ea> */
          a.put(moveImm(DATA, A(11)));
          a.put(moveImm(x, D(3)));
          a.put(move(s, Ea::reg(D(3)), Ea::ind(A(11))));
          a.put(aluToEa(AluOp(op), s, D(2), Ea::ind(A(11))));
          /* and with an immediate */
          a.put(moveImm(x, D(9)));
          a.put(aluImm(AluOp(op), s, y, Ea::reg(D(9))));
          CHECK(!a.bad && run(a) == QEC64_STOPPED, "alu %d.%u assembled", op, n);
          CHECK((R(D(13)) & m) == want && get(DATA, n) == want && (R(D(9)) & m) == want,
                "alu %d.%u %016" PRIx64 " %016" PRIx64 ": %016" PRIx64 " mem %016" PRIx64 " imm %016" PRIx64 " want %016" PRIx64,
                op, n, x, y, R(D(13)) & m, get(DATA, n), R(D(9)) & m, want);
        }
  /* compares and conditions: CMP sets the flags as the host compares */
  const int64_t vs[] = { -5, -1, 0, 1, 7, INT64_MIN, INT64_MAX };
  const Cond conds[] = { CC_EQ, CC_NE, CC_LT, CC_LE, CC_GT, CC_GE, CC_CS, CC_LS, CC_HI, CC_CC };
  for (int64_t x : vs)
    for (int64_t y : vs)
      for (Cond cc : conds) {
        fresh(); Asm a;
        a.put(moveImm(uint64_t(x), D(10)));
        a.put(moveImm(uint64_t(y), D(11)));
        a.put(moveq(0, D(1)));                                    /* before the compare: MOVEQ sets flags */
        a.put(alu(AluOp::Cmp, Size::Q, Ea::reg(D(11)), D(10)));   /* x - y */
        a.put(scc(cc, Ea::reg(D(12))));
        Asm::Label yes;
        a.branch(cc, yes);
        a.put(moveq(1, D(1)));
        a.bind(yes);
        bool want;
        uint64_t ux = uint64_t(x), uy = uint64_t(y);
        switch (cc) {
          case CC_EQ: want = x == y; break; case CC_NE: want = x != y; break;
          case CC_LT: want = x < y; break; case CC_LE: want = x <= y; break;
          case CC_GT: want = x > y; break; case CC_GE: want = x >= y; break;
          case CC_CS: want = ux < uy; break; case CC_LS: want = ux <= uy; break;
          case CC_HI: want = ux > uy; break; default: want = ux >= uy; break;
        }
        CHECK(!a.bad && run(a) == QEC64_STOPPED && (uint8_t(R(D(12))) == (want ? 0xFF : 0)) && (R(D(1)) == 0) == want,
              "cmp %" PRId64 " %" PRId64 " cc %d: scc %02x branch %s", x, y, cc, uint8_t(R(D(12))), R(D(1)) ? "not taken" : "taken");
      }
  /* address arithmetic, ADDQ/SUBQ, unary ops, EXT, SWAP */
  fresh(); { Asm a;
    a.put(moveImm(0x100000000ull, A(9)));
    a.put(moveImm(uint64_t(-16), D(4)));
    a.put(aluAddr(AluOp::Add, Size::Q, Ea::reg(D(4)), A(9)));
    a.put(aluAddr(AluOp::Sub, Size::L, Ea::immediate(0xFFFFFFFF), A(9)));    /* .L sign extends: -(-1) */
    a.put(addq(false, Size::Q, 8, Ea::reg(A(9))));
    a.put(moveImm(5, D(15)));
    a.put(addq(true, Size::Q, 7, Ea::reg(D(15))));
    a.put(moveImm(42, D(8)));
    a.put(unary(UnOp::Neg, Size::Q, Ea::reg(D(8))));
    a.put(moveImm(0xF0, D(7)));
    a.put(unary(UnOp::Not, Size::B, Ea::reg(D(7))));
    a.put(moveImm(0x80, D(6)));
    a.put(ext(Size::L, D(6), true));          /* EXTB.L */
    a.put(moveImm(0x80000000, D(5)));
    a.put(ext(Size::Q, D(5)));                /* EXT.Q */
    a.put(moveImm(0x1111111122222222ull, D(14)));
    a.put(swapHalves(D(14)));
    CHECK(!a.bad && run(a) == QEC64_STOPPED, "addr/unary assembled");
    CHECK(R(A(9)) == 0x100000000ull - 16 + 1 + 8 && R(D(15)) == uint64_t(-2) && R(D(8)) == uint64_t(-42) &&
          (R(D(7)) & 0xFF) == 0x0F && R(D(6)) == 0xFFFFFF80 && R(D(5)) == 0xFFFFFFFF80000000ull &&
          R(D(14)) == 0x2222222211111111ull,
          "adda %" PRIx64 " subq %" PRIx64 " neg %" PRIx64 " not %" PRIx64 " extb %" PRIx64 " ext.q %" PRIx64 " swap %" PRIx64,
          R(A(9)), R(D(15)), R(D(8)), R(D(7)), R(D(6)), R(D(5)), R(D(14)));
  }
  /* shifts at 64 bits, by a register and by an immediate */
  for (unsigned k = 0; k < 64; k += 7) {
    fresh(); Asm a;
    uint64_t v = 0xF00F00F00F00F00Full;
    a.put(moveImm(v, D(9))); a.put(moveImm(v, D(10))); a.put(moveImm(v, D(11))); a.put(moveImm(v, D(12)));
    a.put(moveImm(k, D(3)));
    a.put(shiftReg(ShiftOp::Ls, true, Size::Q, D(3), D(9)));
    a.put(shiftReg(ShiftOp::Ls, false, Size::Q, D(3), D(10)));
    a.put(shiftReg(ShiftOp::As, false, Size::Q, D(3), D(11)));
    a.put(shiftReg(ShiftOp::Ro, true, Size::Q, D(3), D(12)));
    a.put(moveImm(v, D(13)));
    a.put(shiftImm(ShiftOp::Ls, true, Size::Q, 8, D(13)));
    CHECK(!a.bad && run(a) == QEC64_STOPPED && R(D(9)) == v << k && R(D(10)) == v >> k &&
          R(D(11)) == uint64_t(int64_t(v) >> k) && R(D(12)) == (k ? (v << k | v >> (64 - k)) : v) && R(D(13)) == v << 8,
          "shift %u: %" PRIx64 " %" PRIx64 " %" PRIx64 " %" PRIx64, k, R(D(9)), R(D(10)), R(D(11)), R(D(12)));
  }
  /* multiply and divide, 64 and 32 bits */
  const int64_t ms[] = { 3, -7, 1000000007, -123456789012ll, 0x7FFFFFFF };
  for (int64_t x : ms)
    for (int64_t y : ms) {
      fresh(); Asm a;
      a.put(moveImm(uint64_t(x), D(2))); a.put(moveImm(uint64_t(y), D(11)));
      a.put(mulLong(true, Size::Q, Ea::reg(D(11)), D(2)));
      a.put(moveImm(uint64_t(x), D(3)));
      a.put(divLong(true, Size::Q, Ea::reg(D(11)), D(3)));
      a.put(moveImm(uint64_t(x), D(4)));
      a.put(mulLong(false, Size::L, Ea::reg(D(11)), D(4)));
      CHECK(!a.bad && run(a) == QEC64_STOPPED && R(D(2)) == uint64_t(x) * uint64_t(y) && int64_t(R(D(3))) == x / y &&
            uint32_t(R(D(4))) == uint32_t(uint32_t(x) * uint32_t(y)),
            "mul/div %" PRId64 " %" PRId64 ": %" PRIx64 " %" PRId64 " %" PRIx64, x, y, R(D(2)), int64_t(R(D(3))), R(D(4)));
    }
}

static void control()
{
  /* JSR/RTS through a register, LINK/UNLK, BSR over a long distance, CAS */
  fresh(); { Asm a;
    Asm::Label fn, done;
    a.put(moveImm(CODE + 0x4000, A(13)));      /* a subroutine far away, below */
    a.put(jsr(Ea::ind(A(13))));
    a.put(moveImm(1, D(2)));
    a.branch(CC_T, done);
    while (a.here() < 0x4000) a.put(nop());
    a.bind(fn);
    a.put(link(A(6), -32));
    a.put(moveImm(77, D(9)));
    a.put(move(Size::Q, Ea::reg(D(9)), Ea::disp32(A(6), -8)));
    a.put(move(Size::Q, Ea::disp32(A(6), -8), Ea::reg(D(10))));
    a.put(unlk(A(6)));
    a.put(rts());
    a.bind(done);
    a.put(moveImm(DATA, A(2)));
    a.put(moveImm(5, D(3)));
    a.put(move(Size::Q, Ea::reg(D(3)), Ea::ind(A(2))));
    a.put(moveImm(5, D(4))); a.put(moveImm(9, D(5)));
    a.put(cas(Size::Q, D(4), D(5), Ea::ind(A(2))));      /* 5 == 5: stores 9 */
    a.put(moveImm(1, D(6)));
    a.put(cas(Size::Q, D(6), D(5), Ea::ind(A(2))));      /* 1 != 9: D6 = 9 */
    a.put(bitImm(BitOp::Set, 40, Ea::reg(D(5))));
    CHECK(!a.bad && run(a) == QEC64_STOPPED, "control assembled");
    CHECK(R(D(10)) == 77 && R(D(2)) == 1 && c.a[7] == 0x300000 && get(DATA, 8) == 9 && R(D(6)) == 9 &&
          R(D(5)) == (9 | 1ull << 40), "jsr/link %" PRIu64 " sp %" PRIx64 " cas %" PRIu64 " %" PRIu64 " bset %" PRIx64,
          R(D(10)), c.a[7], get(DATA, 8), R(D(6)), R(D(5)));
  }
}

static void fpu()
{
  /* double arithmetic, conversions, compare and branch, as SpiderMonkey's ICs need them */
  const double xs[] = { 1.5, -2.25, 0.0, -0.0, 1e300, std::nan(""), INFINITY };
  for (double x : xs)
    for (double y : xs) {
      fresh(); Asm a;
      uint64_t xb, yb; memcpy(&xb, &x, 8); memcpy(&yb, &y, 8);
      a.put(moveImm(DATA, A(9)));
      a.put(moveImm(xb, D(10))); a.put(move(Size::Q, Ea::reg(D(10)), Ea::ind(A(9))));
      a.put(moveImm(yb, D(11))); a.put(move(Size::Q, Ea::reg(D(11)), Ea::disp32(A(9), 8)));
      a.put(fopEa(F_MOVE, FFmt::D, Ea::ind(A(9)), 0));
      a.put(fopEa(F_MOVE, FFmt::D, Ea::disp32(A(9), 8), 1));
      a.put(fopReg(F_MOVE, 0, 2)); a.put(fopReg(F_ADD, 1, 2));
      a.put(fopReg(F_MOVE, 0, 3)); a.put(fopReg(F_MUL, 1, 3));
      a.put(fopReg(F_MOVE, 0, 4)); a.put(fopReg(F_DIV, 1, 4));
      a.put(fstore(FFmt::D, 2, Ea::disp32(A(9), 16)));
      a.put(fstore(FFmt::D, 3, Ea::disp32(A(9), 24)));
      a.put(fstore(FFmt::D, 4, Ea::disp32(A(9), 32)));
      a.put(fstore(FFmt::L, 0, Ea::reg(D(12))));                 /* int32 by rounding to nearest */
      a.put(moveq(0, D(14)));
      a.put(fopReg(F_CMP, 1, 0));
      a.put(fscc(FC_OLT, Ea::reg(D(13))));
      Asm::Label lt;
      a.fbranch(FC_OLT, lt);
      a.put(moveq(1, D(14)));
      a.bind(lt);
      CHECK(!a.bad && run(a) == QEC64_STOPPED, "fpu assembled");
      auto dbl = [](uint64_t u) { double d; memcpy(&d, &u, 8); return d; };
      auto same = [](double p, double q) { return (p != p && q != q) || memcmp(&p, &q, 8) == 0; };
      double s = dbl(get(DATA + 16, 8)), m = dbl(get(DATA + 24, 8)), q = dbl(get(DATA + 32, 8));
      CHECK(same(s, x + y) && same(m, x * y) && same(q, x / y), "fpu %g %g: %g %g %g", x, y, s, m, q);
      int32_t wantInt = x != x ? 0 : x >= 2147483647.0 ? INT32_MAX : x <= -2147483648.0 ? INT32_MIN : int32_t(std::nearbyint(x));
      CHECK(int32_t(R(D(12))) == wantInt, "fmove.l %g: %d", x, int32_t(R(D(12))));
      bool lessT = x < y;
      CHECK(uint8_t(R(D(13))) == (lessT ? 0xFF : 0) && (R(D(14)) == 0) == lessT, "fcmp %g %g", x, y);
    }
  /* int64 to double and back under the W prefix, FPCR rounding toward zero */
  fresh(); { Asm a;
    a.put(moveImm(uint64_t(-9007199254740993ll), D(9)));
    a.put(fopEa(F_MOVE, FFmt::Q, Ea::reg(D(9)), 5));
    a.put(moveImm(0x10, D(1)));
    a.put(fmoveToFpcr(D(1)));
    a.put(moveImm(0x400C000000000000ull, D(2)));        /* 3.5 */
    a.put(moveImm(DATA, A(1)));
    a.put(move(Size::Q, Ea::reg(D(2)), Ea::ind(A(1))));
    a.put(fopEa(F_MOVE, FFmt::D, Ea::ind(A(1)), 6));
    a.put(fstore(FFmt::Q, 6, Ea::reg(D(3))));
    a.put(fmoveFromFpcr(D(4)));
    CHECK(!a.bad && run(a) == QEC64_STOPPED, "fpu q assembled");
    CHECK(c.fp[5] == double(-9007199254740993ll) && R(D(3)) == 3 && R(D(4)) == 0x10,
          "fmove.q %.17g, fmove to int64 by RZ %" PRId64 ", fpcr %" PRIx64, c.fp[5], int64_t(R(D(3))), R(D(4)));
  }
}

static void refusals()
{
  /* what the CPU has no encoding for comes back with ok == false */
  CHECK(!alu(AluOp::Add, Size::L, Ea::reg(D(1)), A(1)).ok, "ADD to An is ADDA");
  CHECK(!mulLong(true, Size::Q, Ea::reg(D(1)), D(9)).ok, "MULS.L into D9: the extension reaches D0-D7 only");
  CHECK(!cas(Size::Q, D(8), D(1), Ea::ind(A(1))).ok, "CAS with D8");
  CHECK(!move(Size::B, Ea::reg(D(1)), Ea::reg(A(1))).ok, "MOVEA.B");
  CHECK(!lea(Ea::reg(D(1)), A(1)).ok, "LEA of a register");
  CHECK(!fopEa(F_MOVE, FFmt::D, Ea::reg(D(1)), 0).ok, "FMOVE.D from Dn");
}

int main()
{
  ram = static_cast<uint8_t*>(calloc(1, RAM));
  if (!ram) return 2;
  moves();
  arithmetic();
  control();
  fpu();
  refusals();
  printf("%u checks, %u failed\n", checks, failures);
  return failures ? 1 : 0;
}
