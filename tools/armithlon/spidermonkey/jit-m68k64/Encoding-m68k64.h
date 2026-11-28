/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Instruction encodings of the 68100EC64 in long mode (docs/armithlon/68100ec64.md of QRX OS One),
 * for SpiderMonkey's m68k64 backend (docs/armithlon/m68k64-jit.md). Self-contained: no SpiderMonkey
 * types, so the engine's tests run what it encodes on the reference interpreter.
 *
 * Each function returns the instruction's words (Insn). Registers are one space of 32: D0-D15 are
 * 0-15, A0-A15 are 16-31. A register above 7 of its kind needs the prefix word $A0xx before the
 * instruction (W: .L becomes .Q; R: the register in bits 11-9 +8; X: the index register +8;
 * B: the register in bits 2-0 +8); the functions add it.
 *
 * Words are host-order uint16_t; the code buffer stores them big endian.
 */

#ifndef jit_m68k64_Encoding_m68k64_h
#define jit_m68k64_Encoding_m68k64_h

#include <stddef.h>
#include <stdint.h>

namespace js {
namespace jit {
namespace m68k64enc {

using Reg = uint8_t;
static constexpr Reg D(unsigned n) { return Reg(n); }
static constexpr Reg A(unsigned n) { return Reg(16 + n); }
static constexpr bool isAddr(Reg r) { return r >= 16; }
static constexpr unsigned num(Reg r) { return r & 15; }

enum class Size : uint8_t { B = 1, W = 2, L = 4, Q = 8 };

/* The condition codes of Bcc, Scc and DBcc */
enum Cond : uint8_t {
  CC_T = 0, CC_F = 1, CC_HI = 2, CC_LS = 3, CC_CC = 4, CC_CS = 5, CC_NE = 6, CC_EQ = 7,
  CC_VC = 8, CC_VS = 9, CC_PL = 10, CC_MI = 11, CC_GE = 12, CC_LT = 13, CC_GT = 14, CC_LE = 15,
};
static constexpr Cond invert(Cond c) { return Cond(c ^ 1); }

/* An instruction: up to 14 words (prefix, opcode, two full-format extensions with 32-bit displacements,
 * a 64-bit immediate) */
struct Insn {
  uint16_t w[14];
  uint8_t n = 0;
  bool ok = true;     /* false: no such encoding (the caller picked an operand the instruction lacks) */
  void put(uint16_t v) { if (n < 14) w[n++] = v; else ok = false; }
  void put32(uint32_t v) { put(uint16_t(v >> 16)); put(uint16_t(v)); }
  void put64(uint64_t v) { put32(uint32_t(v >> 32)); put32(uint32_t(v)); }
  size_t bytes() const { return size_t(n) * 2; }
};

/* An effective address */
struct Ea {
  enum Kind : uint8_t { DReg, AReg, Ind, PostInc, PreDec, Disp, Index, Abs, PcRel, Imm } kind = DReg;
  Reg base = 0;        /* An (or Dn for DReg); Index: 0xFF is no base */
  Reg index = 0xFF;    /* Index: Dn or An, 0xFF none */
  uint8_t scale = 0;   /* Index: log2 of 1, 2, 4, 8 */
  int32_t disp = 0;    /* Disp, Index, Abs, PcRel (from the extension word) */
  uint64_t imm = 0;

  static Ea reg(Reg r) { Ea e; e.kind = isAddr(r) ? AReg : DReg; e.base = r; return e; }
  static Ea ind(Reg a) { Ea e; e.kind = Ind; e.base = a; return e; }
  static Ea postInc(Reg a) { Ea e; e.kind = PostInc; e.base = a; return e; }
  static Ea preDec(Reg a) { Ea e; e.kind = PreDec; e.base = a; return e; }
  static Ea disp32(Reg a, int32_t d) {
    Ea e; e.base = a; e.disp = d; e.kind = d == 0 ? Ind : Disp; return e;
  }
  static Ea indexed(Reg a, Reg i, unsigned scaleLog2, int32_t d) {
    Ea e; e.kind = Index; e.base = a; e.index = i; e.scale = uint8_t(scaleLog2); e.disp = d; return e;
  }
  static Ea abs(int32_t a) { Ea e; e.kind = Abs; e.disp = a; return e; }
  static Ea pcRel(int32_t d) { Ea e; e.kind = PcRel; e.disp = d; return e; }
  static Ea immediate(uint64_t v) { Ea e; e.kind = Imm; e.imm = v; return e; }
};

/* The prefix bits an operand needs */
enum : uint16_t { PFX_W = 8, PFX_R = 4, PFX_X = 2, PFX_B = 1 };

namespace detail {

struct EaBits {
  uint16_t field;          /* mode << 3 | reg */
  uint16_t pfx;            /* B (the register in the field) and X (the index) */
  uint16_t ext[6];         /* its extension words */
  uint8_t next = 0;
  bool ok = true;
  void put(uint16_t v) { if (next < 6) ext[next++] = v; else ok = false; }
};

/* field: the effective address field; pfxBase: the prefix bit for its register (B for the source
 * field in bits 5-0, R for MOVE's destination field in bits 11-6) */
static inline EaBits encodeEa(const Ea& e, Size s, uint16_t pfxBase = PFX_B) {
  EaBits b{};
  auto rr = [&](Reg r) -> uint16_t {
    if (num(r) >= 8) b.pfx |= pfxBase;
    return uint16_t(num(r) & 7);
  };
  switch (e.kind) {
    case Ea::DReg:
      if (isAddr(e.base)) { b.ok = false; break; }
      b.field = uint16_t(0 << 3) | rr(e.base);
      break;
    case Ea::AReg:
      if (!isAddr(e.base)) { b.ok = false; break; }
      b.field = uint16_t(1 << 3) | rr(e.base);
      break;
    case Ea::Ind: b.field = uint16_t(2 << 3) | rr(e.base); if (!isAddr(e.base)) b.ok = false; break;
    case Ea::PostInc: b.field = uint16_t(3 << 3) | rr(e.base); if (!isAddr(e.base)) b.ok = false; break;
    case Ea::PreDec: b.field = uint16_t(4 << 3) | rr(e.base); if (!isAddr(e.base)) b.ok = false; break;
    case Ea::Disp:
      if (!isAddr(e.base)) { b.ok = false; break; }
      if (e.disp >= -32768 && e.disp <= 32767) {
        b.field = uint16_t(5 << 3) | rr(e.base);
        b.put(uint16_t(int16_t(e.disp)));
      } else {
        /* (bd32,An): the full format, index suppressed */
        b.field = uint16_t(6 << 3) | rr(e.base);
        b.put(0x0170);
        b.put(uint16_t(uint32_t(e.disp) >> 16));
        b.put(uint16_t(e.disp));
      }
      break;
    case Ea::Index: {
      bool noBase = e.base == 0xFF;
      if (!noBase && !isAddr(e.base)) { b.ok = false; break; }
      b.field = uint16_t(6 << 3) | (noBase ? 0 : rr(e.base));
      uint16_t ext = 0;
      if (e.index != 0xFF) {
        if (num(e.index) >= 8) b.pfx |= PFX_X;
        ext |= uint16_t((isAddr(e.index) ? 0x8000 : 0) | (num(e.index) & 7) << 12 | 0x0800 | (e.scale & 3) << 9);
      }
      if (!noBase && e.index != 0xFF && e.disp >= -128 && e.disp <= 127) {
        b.put(uint16_t(ext | uint8_t(int8_t(e.disp))));      /* brief format, index .L */
      } else {
        ext |= 0x0100;                                       /* full format */
        if (noBase) ext |= 0x0080;
        if (e.index == 0xFF) ext |= 0x0040;
        if (e.disp == 0) ext |= 0x0010;
        else if (e.disp >= -32768 && e.disp <= 32767) ext |= 0x0020;
        else ext |= 0x0030;
        b.put(ext);
        if ((ext & 0x30) == 0x20) b.put(uint16_t(int16_t(e.disp)));
        else if ((ext & 0x30) == 0x30) { b.put(uint16_t(uint32_t(e.disp) >> 16)); b.put(uint16_t(e.disp)); }
      }
      break;
    }
    case Ea::Abs:
      if (e.disp >= -32768 && e.disp <= 32767) { b.field = 7 << 3 | 0; b.put(uint16_t(int16_t(e.disp))); }
      else { b.field = 7 << 3 | 1; b.put(uint16_t(uint32_t(e.disp) >> 16)); b.put(uint16_t(e.disp)); }
      break;
    case Ea::PcRel:
      /* (bd32,PC): full format with the index suppressed; the displacement counts from the
       * extension word, so the caller adds the 2 of the opcode word (and the prefix's) */
      if (e.disp >= -32768 && e.disp <= 32767) { b.field = 7 << 3 | 2; b.put(uint16_t(int16_t(e.disp))); }
      else { b.field = 7 << 3 | 3; b.put(0x0170); b.put(uint16_t(uint32_t(e.disp) >> 16)); b.put(uint16_t(e.disp)); }
      break;
    case Ea::Imm:
      b.field = 7 << 3 | 4;
      switch (s) {
        case Size::B: b.put(uint16_t(e.imm & 0xFF)); break;
        case Size::W: b.put(uint16_t(e.imm)); break;
        case Size::L: b.put(uint16_t(e.imm >> 16)); b.put(uint16_t(e.imm)); break;
        case Size::Q:
          b.put(uint16_t(e.imm >> 48)); b.put(uint16_t(e.imm >> 32));
          b.put(uint16_t(e.imm >> 16)); b.put(uint16_t(e.imm));
          break;
      }
      break;
  }
  return b;
}

/* prefix (if any), opcode, extension words of up to two operands */
static inline Insn assemble(uint16_t pfx, uint16_t op, const EaBits* a, const EaBits* b, bool ok = true) {
  Insn i;
  if (pfx) i.put(uint16_t(0xA000 | pfx));
  i.put(op);
  if (a) { for (unsigned k = 0; k < a->next; k++) i.put(a->ext[k]); if (!a->ok) i.ok = false; }
  if (b) { for (unsigned k = 0; k < b->next; k++) i.put(b->ext[k]); if (!b->ok) i.ok = false; }
  if (!ok) i.ok = false;
  return i;
}

/* the size field of most instructions (bits 7-6), and the W prefix for .Q */
static inline uint16_t sz76(Size s) { return s == Size::B ? 0 : s == Size::W ? 1 : 2; }
static inline uint16_t wbit(Size s) { return s == Size::Q ? PFX_W : 0; }
static inline uint16_t rfield(Reg r, uint16_t* pfx) { if (num(r) >= 8) *pfx |= PFX_R; return uint16_t((num(r) & 7) << 9); }

}  // namespace detail

/* ---- moves */

/* MOVE.s src,dst (dst an An: MOVEA, .W .L .Q only; .L sign extends into An) */
static inline Insn move(Size s, const Ea& src, const Ea& dst) {
  using namespace detail;
  EaBits a = encodeEa(src, s, PFX_B), b = encodeEa(dst, s, PFX_R);
  /* both operands use the X bit for an index register above 7: only one of them may have one */
  bool twoIdx = src.kind == Ea::Index && dst.kind == Ea::Index && src.index != 0xFF && dst.index != 0xFF &&
                (num(src.index) >= 8) != (num(dst.index) >= 8);
  uint16_t size = s == Size::B ? 1 : s == Size::W ? 3 : 2;
  uint16_t dfield = uint16_t((b.field & 7) << 9 | (b.field >> 3) << 6);
  bool ok = dst.kind != Ea::Imm && !(dst.kind == Ea::AReg && s == Size::B) && !twoIdx;
  return assemble(uint16_t(a.pfx | b.pfx | wbit(s)), uint16_t(size << 12 | dfield | a.field), &a, &b, ok);
}

/* MOVEQ #-128..127,Dn: sign extends to 64 bits in long mode */
static inline Insn moveq(int8_t v, Reg d) {
  using namespace detail;
  uint16_t pfx = 0;
  uint16_t f = rfield(d, &pfx);
  return assemble(pfx, uint16_t(0x7000 | f | uint8_t(v)), nullptr, nullptr, !isAddr(d));
}

/* The shortest load of a constant: MOVEQ, MOVE.L #imm32 (zero extends), MOVE.Q #imm64 (or to An:
 * MOVEA.L #imm32 sign extends) */
static inline Insn moveImm(uint64_t v, Reg r) {
  if (!isAddr(r) && int64_t(v) >= -128 && int64_t(v) <= 127) return moveq(int8_t(v), r);
  if (!isAddr(r) && v <= 0xFFFFFFFFu) return move(Size::L, Ea::immediate(v), Ea::reg(r));
  if (isAddr(r) && int64_t(v) == int64_t(int32_t(v))) return move(Size::L, Ea::immediate(uint32_t(v)), Ea::reg(r));
  return move(Size::Q, Ea::immediate(v), Ea::reg(r));
}

/* LEA <ea>,An */
static inline Insn lea(const Ea& src, Reg a) {
  using namespace detail;
  EaBits e = encodeEa(src, Size::L);
  uint16_t pfx = e.pfx;
  uint16_t f = rfield(a, &pfx);
  return assemble(pfx, uint16_t(0x41C0 | f | e.field), &e, nullptr, isAddr(a) && src.kind >= Ea::Ind && src.kind != Ea::PostInc && src.kind != Ea::PreDec && src.kind != Ea::Imm);
}

/* PEA <ea>: pushes 8 bytes */
static inline Insn pea(const Ea& src) {
  using namespace detail;
  EaBits e = encodeEa(src, Size::L);
  return assemble(e.pfx, uint16_t(0x4840 | e.field), &e, nullptr);
}

/* EXG: two data, two address, or a data and an address register */
static inline Insn exg(Reg x, Reg y) {
  using namespace detail;
  if (isAddr(x) && !isAddr(y)) { Reg t = x; x = y; y = t; }
  uint16_t pfx = 0, mode = !isAddr(x) && !isAddr(y) ? 0x08 : isAddr(x) && isAddr(y) ? 0x09 : 0x11;
  uint16_t f = rfield(x, &pfx);
  if (num(y) >= 8) pfx |= PFX_B;
  /* the whole registers in long mode (no W needed) */
  return assemble(pfx, uint16_t(0xC100 | f | mode << 3 | (num(y) & 7)), nullptr, nullptr);
}

/* ---- arithmetic and logic */

enum class AluOp : uint8_t { Add, Sub, And, Or, Eor, Cmp };

/* op.s <ea>,Dn (op Dn,<ea> for EOR, which has no other form; see aluToEa) */
static inline Insn alu(AluOp op, Size s, const Ea& src, Reg dn) {
  using namespace detail;
  static const uint16_t line[] = { 0xD000, 0x9000, 0xC000, 0x8000, 0xB100, 0xB000 };
  EaBits e = encodeEa(src, s);
  uint16_t pfx = uint16_t(e.pfx | wbit(s));
  uint16_t f = rfield(dn, &pfx);
  bool ok = !isAddr(dn) && op != AluOp::Eor && !(src.kind == Ea::AReg && (s == Size::B || op == AluOp::And || op == AluOp::Or));
  return assemble(pfx, uint16_t(line[unsigned(op)] | f | sz76(s) << 6 | e.field), &e, nullptr, ok);
}
/* op.s Dn,<ea> (memory destinations, and EOR) */
static inline Insn aluToEa(AluOp op, Size s, Reg dn, const Ea& dst) {
  using namespace detail;
  static const uint16_t line[] = { 0xD100, 0x9100, 0xC100, 0x8100, 0xB100, 0 };
  EaBits e = encodeEa(dst, s);
  uint16_t pfx = uint16_t(e.pfx | wbit(s));
  uint16_t f = rfield(dn, &pfx);
  bool ok = op != AluOp::Cmp && !isAddr(dn) && dst.kind != Ea::AReg && dst.kind != Ea::Imm &&
            (dst.kind != Ea::DReg || op == AluOp::Eor);
  return assemble(pfx, uint16_t(line[unsigned(op)] | f | sz76(s) << 6 | e.field), &e, nullptr, ok);
}
/* ADDA, SUBA, CMPA .s <ea>,An (.W and .L sign extend the source; .Q under W) */
static inline Insn aluAddr(AluOp op, Size s, const Ea& src, Reg an) {
  using namespace detail;
  uint16_t line = op == AluOp::Add ? 0xD000 : op == AluOp::Sub ? 0x9000 : 0xB000;
  EaBits e = encodeEa(src, s);
  uint16_t pfx = uint16_t(e.pfx | wbit(s));
  uint16_t f = rfield(an, &pfx);
  uint16_t opm = s == Size::W ? 0x00C0 : 0x01C0;
  bool ok = isAddr(an) && s != Size::B && (op == AluOp::Add || op == AluOp::Sub || op == AluOp::Cmp);
  return assemble(pfx, uint16_t(line | f | opm | e.field), &e, nullptr, ok);
}
/* ADDI SUBI ANDI ORI EORI CMPI #imm,<ea>; the immediate is 8 bytes for .Q */
static inline Insn aluImm(AluOp op, Size s, uint64_t imm, const Ea& dst) {
  using namespace detail;
  static const uint16_t base[] = { 0x0600, 0x0400, 0x0200, 0x0000, 0x0A00, 0x0C00 };
  Ea im = Ea::immediate(imm);
  EaBits i = encodeEa(im, s), e = encodeEa(dst, s);
  Insn r;
  uint16_t pfx = uint16_t(e.pfx | wbit(s));
  if (pfx) r.put(uint16_t(0xA000 | pfx));
  r.put(uint16_t(base[unsigned(op)] | sz76(s) << 6 | e.field));
  for (unsigned k = 0; k < i.next; k++) r.put(i.ext[k]);
  for (unsigned k = 0; k < e.next; k++) r.put(e.ext[k]);
  r.ok = e.ok && dst.kind != Ea::AReg && dst.kind != Ea::Imm;
  return r;
}
/* ADDQ, SUBQ #1-8,<ea> (on An all 64 bits, no flags) */
static inline Insn addq(bool sub, Size s, unsigned v, const Ea& dst) {
  using namespace detail;
  EaBits e = encodeEa(dst, s);
  return assemble(uint16_t(e.pfx | wbit(s)), uint16_t(0x5000 | (sub ? 0x100 : 0) | (v & 7) << 9 | sz76(s) << 6 | e.field),
                  &e, nullptr, v >= 1 && v <= 8 && !(dst.kind == Ea::AReg && s == Size::B));
}

enum class UnOp : uint8_t { Neg, Not, Clr, Tst };
static inline Insn unary(UnOp op, Size s, const Ea& dst) {
  using namespace detail;
  static const uint16_t base[] = { 0x4400, 0x4600, 0x4200, 0x4A00 };
  EaBits e = encodeEa(dst, s);
  return assemble(uint16_t(e.pfx | wbit(s)), uint16_t(base[unsigned(op)] | sz76(s) << 6 | e.field), &e, nullptr,
                  dst.kind != Ea::AReg);
}
/* EXT: .W from a byte, .L from a word, .Q from a long (EXT.L under W), EXTB.L from a byte */
static inline Insn ext(Size to, Reg dn, bool fromByte = false) {
  using namespace detail;
  uint16_t pfx = num(dn) >= 8 ? PFX_B : 0, op;
  if (to == Size::W) op = 0x4880;
  else if (fromByte) op = 0x49C0;
  else op = 0x48C0;
  if (to == Size::Q) pfx |= PFX_W;
  return assemble(pfx, uint16_t(op | (num(dn) & 7)), nullptr, nullptr, !isAddr(dn) && !(to == Size::Q && fromByte));
}
static inline Insn swapHalves(Reg dn) {   /* SWAP.Q under W: the 32-bit halves */
  using namespace detail;
  return assemble(uint16_t(PFX_W | (num(dn) >= 8 ? PFX_B : 0)), uint16_t(0x4840 | (num(dn) & 7)), nullptr, nullptr, !isAddr(dn));
}

/* Shifts and rotates of a data register by a count in a register or 1-8 */
enum class ShiftOp : uint8_t { As = 0, Ls = 1, Rox = 2, Ro = 3 };
static inline Insn shiftReg(ShiftOp op, bool left, Size s, Reg count, Reg dn) {
  using namespace detail;
  uint16_t pfx = uint16_t(wbit(s) | (num(dn) >= 8 ? PFX_B : 0));
  uint16_t f = rfield(count, &pfx);
  return assemble(pfx, uint16_t(0xE000 | f | (left ? 0x100 : 0) | sz76(s) << 6 | 0x20 | unsigned(op) << 3 | (num(dn) & 7)),
                  nullptr, nullptr, !isAddr(dn) && !isAddr(count));
}
static inline Insn shiftImm(ShiftOp op, bool left, Size s, unsigned n, Reg dn) {
  using namespace detail;
  uint16_t pfx = uint16_t(wbit(s) | (num(dn) >= 8 ? PFX_B : 0));
  return assemble(pfx, uint16_t(0xE000 | (n & 7) << 9 | (left ? 0x100 : 0) | sz76(s) << 6 | unsigned(op) << 3 | (num(dn) & 7)),
                  nullptr, nullptr, n >= 1 && n <= 8 && !isAddr(dn));
}

/* MULS.L, MULU.L <ea>,Dl (64 x 64 under .Q); DIVS.L, DIVU.L <ea>,Dq (64 / 64 under .Q) */
static inline Insn mulLong(bool isSigned, Size s, const Ea& src, Reg dl) {
  using namespace detail;
  EaBits e = encodeEa(src, Size(s == Size::Q ? 8 : 4));
  uint16_t pfx = uint16_t(e.pfx | wbit(s));   /* the extension's register is D0-D7: no prefix reaches it */
  Insn r;
  if (pfx) r.put(uint16_t(0xA000 | pfx));
  r.put(uint16_t(0x4C00 | e.field));
  r.put(uint16_t((num(dl) & 7) << 12 | (isSigned ? 0x0800 : 0)));
  for (unsigned k = 0; k < e.next; k++) r.put(e.ext[k]);
  r.ok = e.ok && !isAddr(dl) && num(dl) < 8 && src.kind != Ea::AReg && (s == Size::L || s == Size::Q);
  return r;
}
static inline Insn divLong(bool isSigned, Size s, const Ea& src, Reg dq) {
  using namespace detail;
  EaBits e = encodeEa(src, Size(s == Size::Q ? 8 : 4));
  uint16_t pfx = uint16_t(e.pfx | wbit(s));
  Insn r;
  if (pfx) r.put(uint16_t(0xA000 | pfx));
  r.put(uint16_t(0x4C40 | e.field));
  /* the remainder register (bits 2-0) is the quotient's: no remainder kept */
  r.put(uint16_t((num(dq) & 7) << 12 | (isSigned ? 0x0800 : 0) | (num(dq) & 7)));
  for (unsigned k = 0; k < e.next; k++) r.put(e.ext[k]);
  r.ok = e.ok && !isAddr(dq) && num(dq) < 8 && src.kind != Ea::AReg && (s == Size::L || s == Size::Q);
  return r;
}

/* ---- control */

/* Bcc.L, BRA.L, BSR.L with a 32-bit displacement from the extension word (the 68020's $FF form);
 * condition CC_T is BRA, CC_F is BSR */
static inline Insn branch32(Cond c, int32_t dispFromExt) {
  Insn r;
  r.put(uint16_t(0x6000 | unsigned(c) << 8 | 0xFF));
  r.put32(uint32_t(dispFromExt));
  return r;
}
static constexpr size_t kBranch32Bytes = 6;
static constexpr size_t kBranch32DispOffset = 2;   /* the displacement's offset in the instruction */

static inline Insn jmp(const Ea& target) {
  using namespace detail;
  EaBits e = encodeEa(target, Size::L);
  return assemble(e.pfx, uint16_t(0x4EC0 | e.field), &e, nullptr);
}
static inline Insn jsr(const Ea& target) {
  using namespace detail;
  EaBits e = encodeEa(target, Size::L);
  return assemble(e.pfx, uint16_t(0x4E80 | e.field), &e, nullptr);
}
static inline Insn rts() { Insn r; r.put(0x4E75); return r; }
static inline Insn nop() { Insn r; r.put(0x4E71); return r; }
static inline Insn illegal() { Insn r; r.put(0x4AFC); return r; }
/* Scc.B <ea>: $FF when the condition holds, else 0 */
static inline Insn scc(Cond c, const Ea& dst) {
  using namespace detail;
  EaBits e = encodeEa(dst, Size::B);
  return assemble(e.pfx, uint16_t(0x50C0 | unsigned(c) << 8 | e.field), &e, nullptr, dst.kind != Ea::AReg);
}
/* LINK.L An,#d32 (pushes An, An = SP, SP += d) and UNLK An */
static inline Insn link(Reg an, int32_t d) {
  Insn r;
  if (num(an) >= 8) r.put(0xA000 | PFX_B);
  r.put(uint16_t(0x4808 | (num(an) & 7)));
  r.put32(uint32_t(d));
  r.ok = isAddr(an);
  return r;
}
static inline Insn unlk(Reg an) {
  Insn r;
  if (num(an) >= 8) r.put(0xA000 | PFX_B);
  r.put(uint16_t(0x4E58 | (num(an) & 7)));
  r.ok = isAddr(an);
  return r;
}

/* CAS.s Dc,Du,<ea>: atomic compare and swap (Dc holds the old value afterwards) */
static inline Insn cas(Size s, Reg dc, Reg du, const Ea& dst) {
  using namespace detail;
  EaBits e = encodeEa(dst, s);
  uint16_t sizeBits = s == Size::B ? 1 : s == Size::W ? 2 : 3;
  uint16_t pfx = uint16_t(e.pfx | wbit(s));
  Insn r;
  /* Du is in the extension's bits 8-6 and Dc in 2-0: the prefix extends neither, so both are D0-D7 */
  if (pfx) r.put(uint16_t(0xA000 | pfx));
  r.put(uint16_t(0x08C0 | sizeBits << 9 | e.field));
  r.put(uint16_t((num(du) & 7) << 6 | (num(dc) & 7)));
  for (unsigned k = 0; k < e.next; k++) r.put(e.ext[k]);
  r.ok = e.ok && !isAddr(dc) && !isAddr(du) && num(dc) < 8 && num(du) < 8 && dst.kind >= Ea::Ind && dst.kind != Ea::Imm;
  return r;
}

/* BTST, BSET, BCLR, BCHG with the bit number in Dn, or immediate */
enum class BitOp : uint8_t { Tst = 0, Chg = 1, Clr = 2, Set = 3 };
static inline Insn bitReg(BitOp op, Reg bitNo, const Ea& dst) {
  using namespace detail;
  EaBits e = encodeEa(dst, Size::B);
  /* on a data register the bit number runs to 63 under W (else modulo 32); in memory modulo 8 */
  uint16_t pfx = uint16_t(e.pfx | (dst.kind == Ea::DReg ? PFX_W : 0));
  uint16_t f = rfield(bitNo, &pfx);
  return assemble(pfx, uint16_t(0x0100 | f | unsigned(op) << 6 | e.field), &e, nullptr, !isAddr(bitNo));
}
static inline Insn bitImm(BitOp op, unsigned bitNo, const Ea& dst) {
  using namespace detail;
  EaBits e = encodeEa(dst, Size::B);
  uint16_t pfx = uint16_t(e.pfx | (dst.kind == Ea::DReg ? PFX_W : 0));   /* bits 32-63 of Dn under W */
  Insn r;
  if (pfx) r.put(uint16_t(0xA000 | pfx));
  r.put(uint16_t(0x0800 | unsigned(op) << 6 | e.field));
  r.put(uint16_t(bitNo & 63));
  for (unsigned k = 0; k < e.next; k++) r.put(e.ext[k]);
  r.ok = e.ok;
  return r;
}

/* ---- the FPU (docs/armithlon/68100ec64.md "FPU"): FP0-FP7 */

using FReg = uint8_t;
/* source formats of <ea> operands */
enum class FFmt : uint8_t { L = 0, S = 1, W = 4, D = 5, B = 6, Q = 8 /* L under the W prefix */ };
enum FOp : uint8_t {
  F_MOVE = 0x00, F_INT = 0x01, F_INTRZ = 0x03, F_SQRT = 0x04, F_ABS = 0x18, F_NEG = 0x1A,
  F_DIV = 0x20, F_ADD = 0x22, F_MUL = 0x23, F_SUB = 0x28, F_CMP = 0x38, F_TST = 0x3A,
  F_SMOVE = 0x40, F_SADD = 0x62, F_SMUL = 0x63, F_SDIV = 0x60, F_SSUB = 0x68, F_SSQRT = 0x41,
};
/* FPU conditions of FBcc and FScc (the 68881's IEEE-aware ones) */
enum FCond : uint8_t {
  FC_F = 0, FC_EQ = 1, FC_OGT = 2, FC_OGE = 3, FC_OLT = 4, FC_OLE = 5, FC_OGL = 6, FC_OR = 7,
  FC_UN = 8, FC_UEQ = 9, FC_UGT = 10, FC_UGE = 11, FC_ULT = 12, FC_ULE = 13, FC_NE = 14, FC_T = 15,
};

static inline Insn fopReg(FOp op, FReg src, FReg dst) {
  Insn r;
  r.put(0xF200);
  r.put(uint16_t((src & 7) << 10 | (dst & 7) << 7 | op));
  return r;
}
static inline Insn fopEa(FOp op, FFmt fmt, const Ea& src, FReg dst) {
  using namespace detail;
  Size s = fmt == FFmt::D || fmt == FFmt::Q ? Size::Q : fmt == FFmt::W ? Size::W : fmt == FFmt::B ? Size::B : Size::L;
  EaBits e = encodeEa(src, s);
  uint16_t pfx = uint16_t(e.pfx | (fmt == FFmt::Q ? PFX_W : 0));
  uint16_t f = uint16_t(fmt == FFmt::Q ? 0 : unsigned(fmt));
  Insn r;
  if (pfx) r.put(uint16_t(0xA000 | pfx));
  r.put(uint16_t(0xF200 | e.field));
  r.put(uint16_t(0x4000 | f << 10 | (dst & 7) << 7 | op));
  for (unsigned k = 0; k < e.next; k++) r.put(e.ext[k]);
  r.ok = e.ok && src.kind != Ea::AReg && !(src.kind == Ea::DReg && fmt == FFmt::D);
  return r;
}
/* FMOVE.fmt FPn,<ea> */
static inline Insn fstore(FFmt fmt, FReg src, const Ea& dst) {
  using namespace detail;
  Size s = fmt == FFmt::D || fmt == FFmt::Q ? Size::Q : fmt == FFmt::W ? Size::W : fmt == FFmt::B ? Size::B : Size::L;
  EaBits e = encodeEa(dst, s);
  uint16_t pfx = uint16_t(e.pfx | (fmt == FFmt::Q ? PFX_W : 0));
  uint16_t f = uint16_t(fmt == FFmt::Q ? 0 : unsigned(fmt));
  Insn r;
  if (pfx) r.put(uint16_t(0xA000 | pfx));
  r.put(uint16_t(0xF200 | e.field));
  r.put(uint16_t(0x6000 | f << 10 | (src & 7) << 7));
  for (unsigned k = 0; k < e.next; k++) r.put(e.ext[k]);
  r.ok = e.ok && dst.kind != Ea::AReg && dst.kind != Ea::Imm && !(dst.kind == Ea::DReg && fmt == FFmt::D);
  return r;
}
/* FBcc.L with a 32-bit displacement from the extension word */
static inline Insn fbranch32(FCond c, int32_t dispFromExt) {
  Insn r;
  r.put(uint16_t(0xF2C0 | c));
  r.put32(uint32_t(dispFromExt));
  return r;
}
static inline Insn fscc(FCond c, const Ea& dst) {
  using namespace detail;
  EaBits e = encodeEa(dst, Size::B);
  Insn r;
  if (e.pfx) r.put(uint16_t(0xA000 | e.pfx));
  r.put(uint16_t(0xF240 | e.field));
  r.put(uint16_t(c));
  for (unsigned k = 0; k < e.next; k++) r.put(e.ext[k]);
  r.ok = e.ok && dst.kind != Ea::AReg;
  return r;
}
/* FMOVE.L Dn,FPCR and FMOVE.L FPCR,Dn (the rounding mode for FINT and conversions) */
static inline Insn fmoveToFpcr(Reg dn) {
  Insn r;
  if (num(dn) >= 8) r.put(0xA000 | PFX_B);
  r.put(uint16_t(0xF200 | (num(dn) & 7)));
  r.put(0x9000);
  r.ok = !isAddr(dn);
  return r;
}
static inline Insn fmoveFromFpcr(Reg dn) {
  Insn r;
  if (num(dn) >= 8) r.put(0xA000 | PFX_B);
  r.put(uint16_t(0xF200 | (num(dn) & 7)));
  r.put(0xB000);
  r.ok = !isAddr(dn);
  return r;
}

}  // namespace m68k64enc
}  // namespace jit
}  // namespace js

#endif /* jit_m68k64_Encoding_m68k64_h */
