/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * The 68100EC64's assembler (docs/armithlon/m68k64-jit.md): the registers' roles, the conditions,
 * the code buffer, labels, relocations and patching. The instruction bits come from
 * Encoding-m68k64.h (tested on the reference interpreter by the engine's ec64_encoding_tests).
 *
 * Conventions, x86's where they fit:
 *  - A jump or call to a label or to other code is BRA.L, Bcc.L, BSR.L or FBcc.L with a 32-bit
 *    displacement in its last 4 bytes, counted from the start of those 4 bytes. Its "source" offset
 *    is the end of the instruction. An unbound label's uses are chained through these fields.
 *  - LEA (bd32,PC),An to a label: the 32-bit displacement is again the last 4 bytes, counted from
 *    the extension word before it (2 bytes earlier); the chain walks both kinds.
 *  - Patchable data is MOVE.Q #imm64,Rn: the value is the last 8 bytes, the label after it.
 *  - The toggled jump or call is BRA.L/BSR.L or CMPI.L #imm32,D0 (the same 6 bytes, the first
 *    word differs).
 */

#ifndef jit_m68k64_Assembler_m68k64_h
#define jit_m68k64_Assembler_m68k64_h

#include "mozilla/Assertions.h"
#include "mozilla/Vector.h"

#include <iterator>
#include <stdint.h>

#include "jit/CompactBuffer.h"
#include "jit/JitCode.h"
#include "jit/m68k64/Architecture-m68k64.h"
#include "jit/m68k64/Encoding-m68k64.h"
#include "jit/ProcessExecutableMemory.h"
#include "jit/Registers.h"
#include "jit/RegisterSets.h"
#include "jit/shared/Assembler-shared.h"
#include "jit/shared/Disassembler-shared.h"
#include "jit/shared/IonAssemblerBuffer.h"  // BufferOffset
#include "js/AllocPolicy.h"
#include "wasm/WasmTypeDecls.h"

namespace js {
namespace jit {

static constexpr Register d0{Registers::d0};
static constexpr Register d1{Registers::d1};
static constexpr Register d2{Registers::d2};
static constexpr Register d3{Registers::d3};
static constexpr Register d4{Registers::d4};
static constexpr Register d5{Registers::d5};
static constexpr Register d6{Registers::d6};
static constexpr Register d7{Registers::d7};
static constexpr Register d8{Registers::d8};
static constexpr Register d9{Registers::d9};
static constexpr Register d10{Registers::d10};
static constexpr Register d11{Registers::d11};
static constexpr Register d12{Registers::d12};
static constexpr Register d13{Registers::d13};
static constexpr Register d14{Registers::d14};
static constexpr Register d15{Registers::d15};
static constexpr Register a0{Registers::a0};
static constexpr Register a1{Registers::a1};
static constexpr Register a2{Registers::a2};
static constexpr Register a3{Registers::a3};
static constexpr Register a4{Registers::a4};
static constexpr Register a5{Registers::a5};
static constexpr Register a6{Registers::a6};
static constexpr Register a7{Registers::a7};
static constexpr Register a8{Registers::a8};
static constexpr Register a9{Registers::a9};
static constexpr Register a10{Registers::a10};
static constexpr Register a11{Registers::a11};
static constexpr Register a12{Registers::a12};
static constexpr Register a13{Registers::a13};
static constexpr Register a14{Registers::a14};
static constexpr Register a15{Registers::a15};

static constexpr FloatRegister fp0{FloatRegisters::f0, FloatRegisters::Double};
static constexpr FloatRegister fp1{FloatRegisters::f1, FloatRegisters::Double};
static constexpr FloatRegister fp2{FloatRegisters::f2, FloatRegisters::Double};
static constexpr FloatRegister fp3{FloatRegisters::f3, FloatRegisters::Double};
static constexpr FloatRegister fp4{FloatRegisters::f4, FloatRegisters::Double};
static constexpr FloatRegister fp5{FloatRegisters::f5, FloatRegisters::Double};
static constexpr FloatRegister fp6{FloatRegisters::f6, FloatRegisters::Double};
static constexpr FloatRegister fp7{FloatRegisters::f7, FloatRegisters::Double};

static constexpr Register InvalidReg{Registers::invalid_reg};
static constexpr FloatRegister InvalidFloatReg;

static constexpr Register StackPointer{Registers::sp};
static constexpr Register FramePointer{Registers::fp};

// The MacroAssembler's scratch registers (not allocatable): ScratchReg for the shared code, D14 for
// the MacroAssembler's own data temporaries, A15 for addresses it forms.
static constexpr Register ScratchReg = d15;
static constexpr Register ScratchReg2 = d14;
static constexpr Register ScratchAddrReg = a15;

static constexpr Register ReturnReg = d0;
static constexpr Register64 ReturnReg64(d0);
static constexpr FloatRegister ReturnFloat32Reg{FloatRegisters::f0, FloatRegisters::Single};
static constexpr FloatRegister ReturnDoubleReg = fp0;
static constexpr FloatRegister ReturnSimd128Reg = InvalidFloatReg;
static constexpr FloatRegister ScratchFloat32Reg{FloatRegisters::f7, FloatRegisters::Single};
static constexpr FloatRegister ScratchDoubleReg = fp7;
static constexpr FloatRegister ScratchSimd128Reg = InvalidFloatReg;

struct ScratchFloat32Scope : public AutoFloatRegisterScope {
  explicit ScratchFloat32Scope(MacroAssembler& masm)
      : AutoFloatRegisterScope(masm, ScratchFloat32Reg) {}
};
struct ScratchDoubleScope : public AutoFloatRegisterScope {
  explicit ScratchDoubleScope(MacroAssembler& masm)
      : AutoFloatRegisterScope(masm, ScratchDoubleReg) {}
};
struct ScratchRegisterScope : public AutoRegisterScope {
  explicit ScratchRegisterScope(MacroAssembler& masm) : AutoRegisterScope(masm, ScratchReg) {}
};
struct SecondScratchRegisterScope : public AutoRegisterScope {
  explicit SecondScratchRegisterScope(MacroAssembler& masm)
      : AutoRegisterScope(masm, ScratchReg2) {}
};

static constexpr Register OsrFrameReg = a3;
static constexpr Register PreBarrierReg = a1;
static constexpr Register InterpreterPCReg = a5;

static constexpr Register CallTempReg0 = a2;
static constexpr Register CallTempReg1 = a3;
static constexpr Register CallTempReg2 = a4;
static constexpr Register CallTempReg3 = d2;
static constexpr Register CallTempReg4 = d3;
static constexpr Register CallTempReg5 = d4;
static constexpr Register CallTempNonArgRegs[] = {a2, a3, a4, d2, d3, d4};
static const uint32_t NumCallTempNonArgRegs = std::size(CallTempNonArgRegs);

static constexpr Register RegExpMatcherRegExpReg = CallTempReg0;
static constexpr Register RegExpMatcherStringReg = CallTempReg1;
static constexpr Register RegExpMatcherLastIndexReg = CallTempReg2;
static constexpr Register RegExpExecTestRegExpReg = CallTempReg0;
static constexpr Register RegExpExecTestStringReg = CallTempReg1;
static constexpr Register RegExpSearcherRegExpReg = CallTempReg0;
static constexpr Register RegExpSearcherStringReg = CallTempReg1;
static constexpr Register RegExpSearcherLastIndexReg = CallTempReg2;

// A boxed Value in one register (JS_PUNBOX64).
static constexpr Register JSReturnReg = d0;
static constexpr Register JSReturnReg_Type = JSReturnReg;
static constexpr Register JSReturnReg_Data = JSReturnReg;
static constexpr ValueOperand JSReturnOperand = ValueOperand(JSReturnReg);

// See "ABI special registers" in Assembler-shared.h. The C ABI has no argument registers, so any
// register that a call into C does not touch is a non-argument register.
static constexpr Register ABINonArgReg0 = d2;
static constexpr Register ABINonArgReg1 = d3;
static constexpr Register ABINonArgReg2 = a2;
static constexpr Register ABINonArgReg3 = a3;
static constexpr Register ABINonArgReturnReg0 = d4;
static constexpr Register ABINonArgReturnReg1 = d5;
static constexpr Register ABINonVolatileReg = d6;
static constexpr Register ABINonArgReturnVolatileReg = a1;
static constexpr FloatRegister ABINonArgDoubleReg = fp6;

// Wasm is not compiled on m68k64 yet (wasm::HasPlatformSupport says no); these only keep the
// shared code building.
static constexpr Register InstanceReg = a13;
static constexpr Register HeapReg = a14;
static constexpr Register WasmTableCallScratchReg0 = ABINonArgReg0;
static constexpr Register WasmTableCallScratchReg1 = ABINonArgReg1;
static constexpr Register WasmTableCallSigReg = ABINonArgReg2;
static constexpr Register WasmTableCallIndexReg = ABINonArgReg3;
static constexpr Register WasmCallRefCallScratchReg0 = ABINonArgReg0;
static constexpr Register WasmCallRefCallScratchReg1 = ABINonArgReg1;
static constexpr Register WasmCallRefCallScratchReg2 = ABINonArgReg2;
static constexpr Register WasmCallRefReg = ABINonArgReg3;
static constexpr Register WasmTailCallInstanceScratchReg = ABINonArgReg1;
static constexpr Register WasmTailCallRAScratchReg = ABINonArgReg2;
static constexpr Register WasmTailCallFPScratchReg = ABINonArgReg3;
static constexpr Register WasmJitEntryReturnScratch = d3;

static constexpr uint32_t ABIStackAlignment = 8;
static constexpr uint32_t CodeAlignment = 16;
static constexpr uint32_t JitStackAlignment = 16;
static constexpr uint32_t JitStackValueAlignment = JitStackAlignment / sizeof(Value);
static_assert(JitStackAlignment % sizeof(Value) == 0 && JitStackValueAlignment >= 1,
              "Stack alignment should be a non-zero multiple of sizeof(Value)");
static constexpr uint32_t SimdMemoryAlignment = 16;
static constexpr uint32_t WasmStackAlignment = SimdMemoryAlignment;
static const uint32_t WasmTrapInstructionLength = 2;  // ILLEGAL
static constexpr uint32_t WasmCheckedCallEntryOffset = 0u;

static constexpr Scale ScalePointer = TimesEight;

// An operand of the few shared-code paths that take one: a register or memory.
class Operand {
 public:
  enum Kind { REG, FREG, MEM_REG_DISP, MEM_SCALE };

 private:
  Kind kind_;
  uint8_t base_;
  uint8_t index_ = Registers::invalid_reg;
  Scale scale_ = TimesOne;
  int32_t disp_ = 0;

 public:
  explicit Operand(Register reg) : kind_(REG), base_(reg.code()) {}
  explicit Operand(FloatRegister reg) : kind_(FREG), base_(reg.encoding()) {}
  explicit Operand(const Address& a)
      : kind_(MEM_REG_DISP), base_(a.base.code()), disp_(a.offset) {}
  explicit Operand(const BaseIndex& a)
      : kind_(MEM_SCALE), base_(a.base.code()), index_(a.index.code()),
        scale_(a.scale), disp_(a.offset) {}
  Operand(Register base, int32_t disp) : kind_(MEM_REG_DISP), base_(base.code()), disp_(disp) {}
  Operand(Register base, Imm32 disp) : kind_(MEM_REG_DISP), base_(base.code()), disp_(disp.value) {}

  Kind kind() const { return kind_; }
  Register reg() const { MOZ_ASSERT(kind_ == REG); return Register::FromCode(base_); }
  FloatRegister fpu() const { MOZ_ASSERT(kind_ == FREG); return FloatRegister::FromCode(base_); }
  Address toAddress() const {
    MOZ_ASSERT(kind_ == MEM_REG_DISP);
    return Address(Register::FromCode(base_), disp_);
  }
  BaseIndex toBaseIndex() const {
    MOZ_ASSERT(kind_ == MEM_SCALE);
    return BaseIndex(Register::FromCode(base_), Register::FromCode(index_), scale_, disp_);
  }
  int32_t disp() const { return disp_; }
};

// The C ABI passes every argument on the stack in an 8-byte slot (small values at the slot's high
// address), so the generator only counts slots. Wasm uses the same.
class ABIArgGenerator : public ABIArgGeneratorShared {
  ABIArg current_;

 public:
  explicit ABIArgGenerator(ABIKind kind) : ABIArgGeneratorShared(kind), current_() {}

  ABIArg next(MIRType argType);
  ABIArg& current() { return current_; }
};

// The code buffer: big-endian 16-bit words. An OOM empties it and sets oom_, as x86's does, so
// callers check once at the end.
class M68k64Buffer {
  class AllocPolicy : private SystemAllocPolicy {
   public:
    using SystemAllocPolicy::checkSimulatedOOM;
    using SystemAllocPolicy::free_;
    using SystemAllocPolicy::reportAllocOverflow;
    template <typename T>
    T* pod_realloc(T* p, size_t oldSize, size_t newSize) {
      static_assert(sizeof(T) == 1, "byte vectors only");
      if (MOZ_UNLIKELY(newSize > MaxCodeBytesPerProcess)) {
        return nullptr;
      }
      return SystemAllocPolicy::pod_realloc<T>(p, oldSize, newSize);
    }
    template <typename T>
    T* pod_malloc(size_t numElems) {
      static_assert(sizeof(T) == 1, "byte vectors only");
      if (MOZ_UNLIKELY(numElems > MaxCodeBytesPerProcess)) {
        return nullptr;
      }
      return SystemAllocPolicy::pod_malloc<T>(numElems);
    }
  };

  mozilla::Vector<uint8_t, 256, AllocPolicy> bytes_;
  bool oom_ = false;

  void oomDetected();

 public:
  size_t size() const { return bytes_.length(); }
  bool oom() const { return oom_; }
  bool reserve(size_t n) { return !oom_ && bytes_.reserve(n); }
  bool swap(Vector<uint8_t, 0, SystemAllocPolicy>& other);
  bool append(const uint8_t* p, size_t n) {
    if (MOZ_UNLIKELY(!bytes_.append(p, n))) {
      oomDetected();
      return false;
    }
    return true;
  }
  void put16(uint16_t v) {
    uint8_t b[2] = {uint8_t(v >> 8), uint8_t(v)};
    append(b, 2);
  }
  void put32(uint32_t v) {
    put16(uint16_t(v >> 16));
    put16(uint16_t(v));
  }
  void put64(uint64_t v) {
    put32(uint32_t(v >> 32));
    put32(uint32_t(v));
  }
  uint8_t* data() { return bytes_.begin(); }
  const uint8_t* buffer() const {
    MOZ_RELEASE_ASSERT(!oom_);
    return bytes_.begin();
  }
};

// Big-endian reads and writes of code.
static inline uint16_t ReadCode16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
static inline uint32_t ReadCode32(const uint8_t* p) {
  return uint32_t(ReadCode16(p)) << 16 | ReadCode16(p + 2);
}
static inline uint64_t ReadCode64(const uint8_t* p) {
  return uint64_t(ReadCode32(p)) << 32 | ReadCode32(p + 4);
}
static inline void WriteCode16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
static inline void WriteCode32(uint8_t* p, uint32_t v) {
  WriteCode16(p, uint16_t(v >> 16));
  WriteCode16(p + 2, uint16_t(v));
}
static inline void WriteCode64(uint8_t* p, uint64_t v) {
  WriteCode32(p, uint32_t(v >> 32));
  WriteCode32(p + 4, uint32_t(v));
}

class Assembler : public AssemblerShared {
 public:
  // The integer conditions are the 68k's condition codes. CMP src,Dn sets the flags of Dn - src,
  // as x86's cmp does in AT&T order, so "cmp(rhs, lhs); j(LessThan)" is lhs < rhs.
  enum Condition {
    Equal = m68k64enc::CC_EQ,
    NotEqual = m68k64enc::CC_NE,
    Above = m68k64enc::CC_HI,
    AboveOrEqual = m68k64enc::CC_CC,
    Below = m68k64enc::CC_CS,
    BelowOrEqual = m68k64enc::CC_LS,
    GreaterThan = m68k64enc::CC_GT,
    GreaterThanOrEqual = m68k64enc::CC_GE,
    LessThan = m68k64enc::CC_LT,
    LessThanOrEqual = m68k64enc::CC_LE,
    Overflow = m68k64enc::CC_VS,
    NoOverflow = m68k64enc::CC_VC,
    CarrySet = m68k64enc::CC_CS,
    CarryClear = m68k64enc::CC_CC,
    Signed = m68k64enc::CC_MI,
    NotSigned = m68k64enc::CC_PL,
    Zero = m68k64enc::CC_EQ,
    NonZero = m68k64enc::CC_NE,
    Always = m68k64enc::CC_T,
  };

  // The FPU's predicates (FBcc) cover every IEEE case, so a double condition is one FBcc. FCMP
  // src,FPn compares FPn with src.
  enum DoubleCondition {
    DoubleOrdered = m68k64enc::FC_OR,
    DoubleEqual = m68k64enc::FC_EQ,
    DoubleNotEqual = m68k64enc::FC_OGL,
    DoubleGreaterThan = m68k64enc::FC_OGT,
    DoubleGreaterThanOrEqual = m68k64enc::FC_OGE,
    DoubleLessThan = m68k64enc::FC_OLT,
    DoubleLessThanOrEqual = m68k64enc::FC_OLE,
    DoubleUnordered = m68k64enc::FC_UN,
    DoubleEqualOrUnordered = m68k64enc::FC_UEQ,
    DoubleNotEqualOrUnordered = m68k64enc::FC_NE,
    DoubleGreaterThanOrUnordered = m68k64enc::FC_UGT,
    DoubleGreaterThanOrEqualOrUnordered = m68k64enc::FC_UGE,
    DoubleLessThanOrUnordered = m68k64enc::FC_ULT,
    DoubleLessThanOrEqualOrUnordered = m68k64enc::FC_ULE,
  };

  static Condition InvertCondition(Condition cond) {
    return Condition(m68k64enc::invert(m68k64enc::Cond(cond)));
  }
  static DoubleCondition InvertCondition(DoubleCondition cond) {
    // The 68881's predicates: the complement is the other 4 bits
    return DoubleCondition(cond ^ 0xF);
  }
  // The condition for the swapped operands of a compare.
  static Condition SwapCondition(Condition cond);
  static DoubleCondition SwapCondition(DoubleCondition cond);

  static Condition UnsignedCondition(Condition cond);
  static Condition ConditionWithoutEqual(Condition cond);

 protected:
  struct RelativePatch {
    int32_t offset;  // the end of the BRA.L/BSR.L
    void* target;
    RelocationKind kind;
    RelativePatch(int32_t offset, void* target, RelocationKind kind)
        : offset(offset), target(target), kind(kind) {}
  };

  M68k64Buffer m_buffer;
  CompactBufferWriter jumpRelocations_;
  CompactBufferWriter dataRelocations_;
  Vector<RelativePatch, 8, SystemAllocPolicy> jumps_;

 public:
  Assembler() = default;

  // ---- the buffer
  void emit(const m68k64enc::Insn& i) {
    MOZ_ASSERT(i.ok, "m68k64: an operand the instruction does not have");
    for (unsigned k = 0; k < i.n; k++) {
      m_buffer.put16(i.w[k]);
    }
  }
  void emit16(uint16_t w) { m_buffer.put16(w); }
  void emit32(uint32_t w) { m_buffer.put32(w); }
  void emit64(uint64_t w) { m_buffer.put64(w); }

  uint32_t currentOffset() const { return uint32_t(m_buffer.size()); }
  size_t size() const { return m_buffer.size(); }
  size_t jumpRelocationTableBytes() const { return jumpRelocations_.length(); }
  size_t dataRelocationTableBytes() const { return dataRelocations_.length(); }
  size_t bytesNeeded() const {
    return size() + jumpRelocationTableBytes() + dataRelocationTableBytes();
  }
  bool oom() const {
    return AssemblerShared::oom() || m_buffer.oom() || jumpRelocations_.oom() ||
           dataRelocations_.oom();
  }
  bool reserve(size_t size) { return m_buffer.reserve(size); }
  bool swapBuffer(wasm::Bytes& other) { return m_buffer.swap(other); }
  bool appendRawCode(const uint8_t* code, size_t numBytes) {
    return m_buffer.append(code, numBytes);
  }
  void setUnlimitedBuffer() {}
  void setPrinter(Sprinter* sp) {}
  Register getStackPointer() const { return StackPointer; }

  void executableCopy(void* buffer);
  void processCodeLabels(uint8_t* rawCode);
  void copyJumpRelocationTable(uint8_t* dest);
  void copyDataRelocationTable(uint8_t* dest);
  void finish();

  static void TraceJumpRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader);
  static void TraceDataRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader);

  // Records the MOVE.Q #imm64 just emitted as a GC pointer (or a Value) to trace.
  void writeDataRelocation(ImmGCPtr ptr) {
    if (ptr.value) {
      if (gc::IsInsideNursery(ptr.value)) {
        embedsNurseryPointers_ = true;
      }
      dataRelocations_.writeUnsigned(currentOffset());
    }
  }
  void writeDataRelocation(const Value& val) {
    if (val.isGCThing()) {
      gc::Cell* cell = val.toGCThing();
      if (cell && gc::IsInsideNursery(cell)) {
        embedsNurseryPointers_ = true;
      }
      dataRelocations_.writeUnsigned(currentOffset());
    }
  }

  // ---- alignment
  void haltingAlign(int alignment);
  void nopAlign(int alignment);
  void writeCodePointer(CodeLabel* label) {
    emit64(UINT64_MAX);  // patched after code generation
    label->patchAt()->bind(currentOffset());
  }

  // ---- labels and branches
  void bind(Label* label) { bind(label, BufferOffset(currentOffset())); }
  void bind(Label* label, BufferOffset target);
  void bind(CodeLabel* label) { label->target()->bind(currentOffset()); }
  void retarget(Label* label, Label* target);
  static void Bind(uint8_t* rawCode, const CodeLabel& label);

  // Bcc.L (Always: BRA.L) to a label; returns the source offset.
  uint32_t jSrc(Condition cond, Label* label);
  // FBcc.L to a label.
  uint32_t fjSrc(DoubleCondition cond, Label* label);
  // BSR.L to a label.
  uint32_t callSrc(Label* label);
  // LEA (bd32,PC),An with the label's address.
  void leaLabel(Label* label, Register an);

  void j(Condition cond, Label* label) { jSrc(cond, label); }
  void jmp(Label* label) { jSrc(Always, label); }
  void fj(DoubleCondition cond, Label* label) { fjSrc(cond, label); }

  // A BRA.L/BSR.L to a fixed address, patched in executableCopy (all JIT code is in one region,
  // well within 2 GB); other targets go through A15.
  void jumpTo(ImmPtr target, RelocationKind reloc);
  void callTo(ImmPtr target, RelocationKind reloc);
  void addPendingJump(uint32_t src, ImmPtr target, RelocationKind reloc);

  CodeOffset callWithPatch() {
    emit(m68k64enc::branch32(m68k64enc::CC_F, 0));
    return CodeOffset(currentOffset());
  }
  void patchCall(uint32_t callerOffset, uint32_t calleeOffset) {
    uint8_t* code = m_buffer.data();
    SetRel32(code + callerOffset, code + calleeOffset);
  }
  CodeOffset farJumpWithPatch() {
    emit(m68k64enc::branch32(m68k64enc::CC_T, 0));
    return CodeOffset(currentOffset());
  }
  void patchFarJump(CodeOffset farJump, uint32_t targetOffset) {
    uint8_t* code = m_buffer.data();
    SetRel32(code + farJump.offset(), code + targetOffset);
  }
  static void patchFarJump(uint8_t* farJump, uint8_t* target) { SetRel32(farJump, target); }

  // The 32-bit displacement of the branch ending at `end`.
  static void SetRel32(uint8_t* end, const void* target) {
    intptr_t d = intptr_t(target) - intptr_t(end - 4);
    MOZ_RELEASE_ASSERT(d == int32_t(d));
    WriteCode32(end - 4, uint32_t(int32_t(d)));
  }
  static uint8_t* GetRel32Target(uint8_t* end) {
    return end - 4 + int32_t(ReadCode32(end - 4));
  }
  // The value of the MOVE.Q #imm64 (or the code pointer) ending at `end`.
  static uintptr_t GetPointer(uint8_t* end) { return uintptr_t(ReadCode64(end - 8)); }
  static void SetPointer(uint8_t* end, const void* value) {
    WriteCode64(end - 8, uint64_t(uintptr_t(value)));
  }

  // ---- patching after code generation
  static void PatchDataWithValueCheck(CodeLocationLabel data, PatchedImmPtr newData,
                                      PatchedImmPtr expectedData) {
    uint8_t* end = data.raw();
    MOZ_ASSERT(GetPointer(end) == uintptr_t(expectedData.value));
    SetPointer(end, newData.value);
  }
  static void PatchDataWithValueCheck(CodeLocationLabel data, ImmPtr newData,
                                      ImmPtr expectedData) {
    PatchDataWithValueCheck(data, PatchedImmPtr(newData.value), PatchedImmPtr(expectedData.value));
  }
  // The instruction before the label ends in a 32-bit immediate.
  static void PatchWrite_Imm32(CodeLocationLabel label, Imm32 imm) {
    WriteCode32(label.raw() - 4, uint32_t(imm.value));
  }
  // A BSR.L at `start` to `toCall` (Ion's invalidation of OSI points).
  static void PatchWrite_NearCall(CodeLocationLabel start, CodeLocationLabel toCall);
  static uint32_t PatchWrite_NearCallSize() { return m68k64enc::kBranch32Bytes; }

  static constexpr uint16_t ToggleOffOp = 0x0C80;  // CMPI.L #imm32,D0
  static constexpr uint16_t ToggleJmpOp = 0x60FF;  // BRA.L
  static constexpr uint16_t ToggleCallOp = 0x61FF; // BSR.L
  static void ToggleToJmp(CodeLocationLabel inst) {
    MOZ_ASSERT(ReadCode16(inst.raw()) == ToggleOffOp);
    WriteCode16(inst.raw(), ToggleJmpOp);
  }
  static void ToggleToCmp(CodeLocationLabel inst) {
    MOZ_ASSERT(ReadCode16(inst.raw()) == ToggleJmpOp);
    WriteCode16(inst.raw(), ToggleOffOp);
  }
  static void ToggleCall(CodeLocationLabel inst, bool enabled) {
    MOZ_ASSERT(ReadCode16(inst.raw()) == ToggleOffOp || ReadCode16(inst.raw()) == ToggleCallOp);
    WriteCode16(inst.raw(), enabled ? ToggleCallOp : ToggleOffOp);
  }
  static size_t ToggledCallSize(uint8_t* code) { return m68k64enc::kBranch32Bytes; }

  // A jump that starts off (CMPI.L) and is turned on with ToggleToJmp.
  CodeOffset toggledJump(Label* label) {
    CodeOffset offset(currentOffset());
    jSrc(Always, label);
    WriteCode16(m_buffer.data() + offset.offset(), ToggleOffOp);
    return offset;
  }
  CodeOffset toggledCall(JitCode* target, bool enabled) {
    CodeOffset offset(currentOffset());
    callTo(ImmPtr(target->raw()), RelocationKind::JITCODE);
    if (!enabled && !oom()) {
      WriteCode16(m_buffer.data() + offset.offset(), ToggleOffOp);
    }
    return offset;
  }

  static bool HasRoundInstruction(RoundingMode mode) {
    // FINT with the rounding mode in FPCR; FINTRZ truncates.
    switch (mode) {
      case RoundingMode::Up:
      case RoundingMode::Down:
      case RoundingMode::NearestTiesToEven:
      case RoundingMode::TowardsZero:
        return true;
    }
    MOZ_CRASH("unexpected mode");
  }

  void verifyHeapAccessDisassembly(uint32_t begin, uint32_t end,
                                   const Disassembler::HeapAccess& heapAccess) {}

  // ---- effective addresses
  static m68k64enc::Reg R(Register r) { return m68k64enc::Reg(r.code()); }
  static m68k64enc::FReg F(FloatRegister f) { return m68k64enc::FReg(f.encoding()); }
  static bool IsAddr(Register r) { return Registers::IsAddress(r.code()); }

  // (d,An), or (bd32,ZA,Dn.L) when the base is a data register.
  static m68k64enc::Ea ea(const Address& a) {
    using m68k64enc::Ea;
    if (IsAddr(a.base)) {
      return a.offset == 0 ? Ea::ind(R(a.base)) : Ea::disp32(R(a.base), a.offset);
    }
    return Ea::indexed(0xFF, R(a.base), 0, a.offset);
  }
  // (bd,An,Xn*s). With a data register as the base, the two swap when the scale is 1; otherwise the
  // base goes to A15 first (LEA), which `ea` emits.
  m68k64enc::Ea ea(const BaseIndex& a);
  m68k64enc::Ea ea(Register r) { return m68k64enc::Ea::reg(R(r)); }
  m68k64enc::Ea ea(const Operand& op);
  static m68k64enc::Ea imm(uint64_t v) { return m68k64enc::Ea::immediate(v); }

  // ---- a few instructions the shared code calls directly
  void nop() { emit(m68k64enc::nop()); }
  void nop(size_t bytes) {
    MOZ_ASSERT(bytes % 2 == 0);
    for (size_t i = 0; i < bytes; i += 2) {
      nop();
    }
  }
  void breakpoint() { emit(m68k64enc::illegal()); }  // an exception AROS reports with a backtrace
  CodeOffset illegal() {
    CodeOffset off(currentOffset());
    emit(m68k64enc::illegal());
    return off;
  }
  BufferOffset ret() {
    BufferOffset off(currentOffset());
    emit(m68k64enc::rts());
    return off;
  }

  static uint32_t NopSize() { return 2; }
};

static inline bool GetTempRegForIntArg(uint32_t usedIntArgs, uint32_t usedFloatArgs, Register* out) {
  // No argument registers: every argument is on the stack.
  return false;
}

}  // namespace jit
}  // namespace js

#endif /* jit_m68k64_Assembler_m68k64_h */
