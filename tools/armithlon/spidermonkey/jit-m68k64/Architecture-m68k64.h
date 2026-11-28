/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * The 68100EC64 in long mode (QRX OS One 64), docs/armithlon/m68k64-jit.md.
 *
 * Integer registers: D0-D15 (codes 0-15) and A0-A15 (codes 16-31), 64 bits each. Arithmetic and
 * logic take D registers, addressing modes take A registers as bases; the MacroAssembler copies
 * between the two kinds where an instruction needs the other. A call pushes its return address on
 * the stack, as on x86.
 *
 * The C ABI (tools/armithlon/ec64-llvm, version 0): arguments on the stack in 8-byte slots, results
 * in D0/D1 (pointers in A0), D2-D7 and A2-A6 kept by the callee. The compiler does not use D8-D15
 * and A8-A15 at all, so a C call clobbers D0, D1, A0 and A1 only. AROS keeps its per-core data in
 * A8, and saves all 32 registers and the FPU with a task.
 *
 * The FPU: FP0-FP7, double precision (single precision results through the FS* instructions).
 */

#ifndef jit_m68k64_Architecture_m68k64_h
#define jit_m68k64_Architecture_m68k64_h

#include <algorithm>
#include <bit>

#include "jit/shared/Architecture-shared.h"

#include "js/Utility.h"

namespace js {
namespace jit {

class Registers {
 public:
  enum RegisterID {
    d0 = 0, d1, d2, d3, d4, d5, d6, d7, d8, d9, d10, d11, d12, d13, d14, d15,
    a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15,
    sp = a7,
    fp = a6,
    tls = a8,          // AROS's per-core data: never touched
    invalid_reg,
  };
  typedef uint8_t Code;
  typedef RegisterID Encoding;
  typedef uint32_t SetType;

  static const Encoding StackPointer = sp;
  static const Encoding Invalid = invalid_reg;

  // Content spilled during bailouts.
  union RegisterContent {
    uintptr_t r;
  };

  static uint32_t SetSize(SetType x) { return std::popcount(x); }
  static uint32_t FirstBit(SetType x) {
    MOZ_ASSERT(x);
    return std::countr_zero(x);
  }
  static uint32_t LastBit(SetType x) {
    MOZ_ASSERT(x);
    return std::bit_width(x) - 1;
  }

  static const char* GetName(uint32_t code) {
    static const char* const Names[] = {
        "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8", "d9", "d10",
        "d11", "d12", "d13", "d14", "d15", "a0", "a1", "a2", "a3", "a4", "a5",
        "a6", "sp", "a8", "a9", "a10", "a11", "a12", "a13", "a14", "a15"};
    static_assert(Total == std::size(Names), "Table is the correct size");
    if (code >= Total) {
      return "invalid";
    }
    return Names[code];
  }

  static Code FromName(const char* name);

  static bool IsAddress(Code c) { return c >= a0; }

  static const uint32_t Total = 32;
  static const uint32_t TotalPhys = 32;

  static const SetType AllMask = 0xFFFFFFFF;
  static const SetType NoneMask = 0x0;

  // No argument registers: the C ABI passes everything on the stack.
  static const SetType ArgRegMask = 0;

  // What a call into C clobbers.
  static const SetType VolatileMask =
      (1U << d0) | (1U << d1) | (1U << a0) | (1U << a1);

  static const SetType NonVolatileMask = AllMask & ~VolatileMask;

  static const SetType NonAllocatableMask =
      (1U << sp) | (1U << fp) | (1U << tls) |
      (1U << d15) | (1U << d14) |   // the MacroAssembler's data scratch registers
      (1U << a15);                  // and its address scratch register

  static const uint32_t Allocatable = 32 - 6;

  static const SetType WrapperMask = VolatileMask;

  // Registers returned from a JS -> JS call (a boxed Value).
  static const SetType JSCallMask = (1U << d0);

  // Registers returned from a JS -> C call.
  static const SetType CallMask = (1U << d0) | (1U << a0);

  static const SetType AllocatableMask = AllMask & ~NonAllocatableMask;
};

// Smallest integer type that can hold a register bitmask.
typedef uint32_t PackedRegisterMask;

template <typename T>
class TypedRegisterSet;

class FloatRegisters {
 public:
  enum FPRegisterID { f0 = 0, f1, f2, f3, f4, f5, f6, f7 };

  // (invalid << 7) | (kind << 3) | encoding
  typedef uint8_t Code;
  typedef FPRegisterID Encoding;
  typedef uint32_t SetType;

  enum Kind : uint8_t { Double, Single, NumTypes };

  static constexpr Code Invalid = 0x80;

  static const char* GetName(uint32_t code) {
    static const char* const Names[] = {"fp0", "fp1", "fp2", "fp3",
                                        "fp4", "fp5", "fp6", "fp7"};
    if (code >= Total) {
      return "invalid";
    }
    // Single and double registers are the same registers.
    return Names[code % TotalPhys];
  }

  static Code FromName(const char* name);

  static const uint32_t TotalPhys = 8;
  static const uint32_t Total = TotalPhys * NumTypes;
  static const uint32_t Allocatable = 7;  // FP7 is the scratch register

  static_assert(sizeof(SetType) * 8 >= Total,
                "SetType should be large enough to enumerate all registers.");

  static const SetType SpreadSingle = SetType(1) << (uint32_t(Single) * TotalPhys);
  static const SetType SpreadDouble = SetType(1) << (uint32_t(Double) * TotalPhys);
  static const SetType Spread = SpreadSingle | SpreadDouble;

  static const SetType AllPhysMask = ((SetType(1) << TotalPhys) - 1);
  static const SetType AllMask = AllPhysMask * Spread;
  static const SetType AllSingleMask = AllPhysMask * SpreadSingle;
  static const SetType AllDoubleMask = AllPhysMask * SpreadDouble;
  static const SetType NoneMask = SetType(0);

  // C code does not use the FPU (soft float): a call into C keeps every FP register.
  static const SetType NonVolatileMask = AllMask;
  static const SetType VolatileMask = SetType(0);
  static const SetType WrapperMask = VolatileMask;

  static const SetType NonAllocatableMask = (SetType(1) << f7) * Spread;
  static const SetType AllocatableMask = AllMask & ~NonAllocatableMask;

  // Content spilled during bailouts.
  union RegisterContent {
    float s;
    double d;
  };

  static constexpr Encoding encoding(Code c) { return Encoding(c & 7); }
  static constexpr Kind kind(Code c) { return Kind((c >> 3) & 3); }
  static constexpr Code fromParts(uint32_t encoding, uint32_t kind, uint32_t invalid) {
    return Code((invalid << 7) | (kind << 3) | encoding);
  }
};

static const uint32_t SpillSlotSize =
    std::max(sizeof(Registers::RegisterContent),
             sizeof(FloatRegisters::RegisterContent));

static constexpr uint32_t ShadowStackSpace = 0;
// JSR and BSR push the return address, as x86's CALL does.
static const uint32_t SizeOfReturnAddressAfterCall = sizeof(void*);

// Bcc.L, BSR.L and the (d32,PC) forms reach 2 GB either way: no jump islands.
static const uint32_t JumpImmediateRange = UINT32_C(0x7FFFFFFF);

struct FloatRegister {
  typedef FloatRegisters Codes;
  typedef size_t Code;
  typedef Codes::Encoding Encoding;
  typedef Codes::SetType SetType;

  static uint32_t SetSize(SetType x) {
    x |= x >> FloatRegisters::TotalPhys;
    x &= FloatRegisters::AllPhysMask;
    return std::popcount(x);
  }
  static uint32_t FirstBit(SetType x) {
    MOZ_ASSERT(x);
    return std::countr_zero(x);
  }
  static uint32_t LastBit(SetType x) {
    MOZ_ASSERT(x);
    return std::bit_width(x) - 1;
  }

 private:
  uint8_t encoding_;  // FP0-FP7
  uint8_t kind_;      // Double, Single
  bool invalid_;

  typedef Codes::Kind Kind;

 public:
  constexpr FloatRegister(Encoding encoding, Kind kind)
      : encoding_(encoding), kind_(kind), invalid_(false) {}
  constexpr FloatRegister()
      : encoding_(0), kind_(FloatRegisters::Double), invalid_(true) {}

  static FloatRegister FromCode(uint32_t i) {
    MOZ_ASSERT(i < Codes::Total);
    return FloatRegister(FloatRegisters::encoding(i), FloatRegisters::kind(i));
  }

  bool isSingle() const { MOZ_ASSERT(!invalid_); return kind_ == FloatRegisters::Single; }
  bool isDouble() const { MOZ_ASSERT(!invalid_); return kind_ == FloatRegisters::Double; }
  bool isSimd128() const { MOZ_ASSERT(!invalid_); return false; }
  bool isInvalid() const { return invalid_; }

  FloatRegister asSingle() const { return FloatRegister(Encoding(encoding_), FloatRegisters::Single); }
  FloatRegister asDouble() const { return FloatRegister(Encoding(encoding_), FloatRegisters::Double); }
  FloatRegister asSimd128() const { MOZ_CRASH(); }

  constexpr uint32_t size() const {
    return kind_ == FloatRegisters::Double ? sizeof(double) : sizeof(float);
  }
  constexpr Code code() const { return Codes::fromParts(encoding_, kind_, invalid_); }
  constexpr Encoding encoding() const { return Encoding(encoding_); }

  const char* name() const { return FloatRegisters::GetName(code()); }
  bool volatile_() const {
    return !!((SetType(1) << code()) & FloatRegisters::VolatileMask);
  }
  constexpr bool operator!=(FloatRegister other) const { return code() != other.code(); }
  constexpr bool operator==(FloatRegister other) const { return code() == other.code(); }

  bool aliases(FloatRegister other) const { return other.encoding_ == encoding_; }
  bool equiv(FloatRegister other) const { return kind_ == other.kind_; }

  uint32_t numAliased() const { return Codes::NumTypes; }
  uint32_t numAlignedAliased() { return numAliased(); }

  FloatRegister aliased(uint32_t aliasIdx) {
    MOZ_ASSERT(aliasIdx < numAliased());
    return FloatRegister(Encoding(encoding_), Kind((aliasIdx + kind_) % numAliased()));
  }
  FloatRegister alignedAliased(uint32_t aliasIdx) { return aliased(aliasIdx); }
  SetType alignedOrDominatedAliasedSet() const { return Codes::Spread << encoding_; }

  static constexpr RegTypeName DefaultType = RegTypeName::Float64;

  template <RegTypeName Name = DefaultType>
  static SetType LiveAsIndexableSet(SetType s) {
    return SetType(0);
  }
  template <RegTypeName Name = DefaultType>
  static SetType AllocatableAsIndexableSet(SetType s) {
    static_assert(Name != RegTypeName::Any, "Allocatable set are not iterable");
    return LiveAsIndexableSet<Name>(s);
  }

  static TypedRegisterSet<FloatRegister> ReduceSetForPush(const TypedRegisterSet<FloatRegister>& s);
  static uint32_t GetPushSizeInBytes(const TypedRegisterSet<FloatRegister>& s);
  uint32_t getRegisterDumpOffsetInBytes();
};

template <>
inline FloatRegister::SetType FloatRegister::LiveAsIndexableSet<RegTypeName::Float32>(SetType set) {
  return set & FloatRegisters::AllSingleMask;
}
template <>
inline FloatRegister::SetType FloatRegister::LiveAsIndexableSet<RegTypeName::Float64>(SetType set) {
  return set & FloatRegisters::AllDoubleMask;
}
template <>
inline FloatRegister::SetType FloatRegister::LiveAsIndexableSet<RegTypeName::Any>(SetType set) {
  return set;
}

// A double register can always be used as a float32 one, and aliases a single float.
inline bool hasUnaliasedDouble() { return false; }
inline bool hasMultiAlias() { return false; }

}  // namespace jit
}  // namespace js

#endif /* jit_m68k64_Architecture_m68k64_h */
