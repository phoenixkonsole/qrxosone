/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/m68k64/Assembler-m68k64.h"

#include "mozilla/Maybe.h"

#include <string.h>

#include "gc/Marking.h"
#include "gc/Tracer.h"
#include "jit/AutoWritableJitCode.h"
#include "jit/ExecutableAllocator.h"

using namespace js;
using namespace js::jit;

using mozilla::Maybe;

namespace enc = js::jit::m68k64enc;

// ---- the ABI

ABIArg ABIArgGenerator::next(MIRType type) {
  switch (type) {
    case MIRType::Int32:
    case MIRType::Float32:
      // 32-bit values sit in the slot's high half (big endian)
      current_ = ABIArg(stackOffset_ + 4);
      stackOffset_ += 8;
      break;
    case MIRType::Pointer:
    case MIRType::Int64:
    case MIRType::Double:
    case MIRType::WasmAnyRef:
    case MIRType::WasmArrayData:
    case MIRType::StackResults:
      current_ = ABIArg(stackOffset_);
      stackOffset_ += 8;
      break;
    default:
      MOZ_CRASH("m68k64: unexpected argument type");
  }
  return current_;
}

// ---- the buffer

void M68k64Buffer::oomDetected() {
  oom_ = true;
  bytes_.clear();
#ifdef DEBUG
  JitContext* context = MaybeGetJitContext();
  if (context) {
    context->setOOM();
  }
#endif
}

bool M68k64Buffer::swap(Vector<uint8_t, 0, SystemAllocPolicy>& other) {
  MOZ_ASSERT(other.empty());
  if (oom_) {
    return false;
  }
  if (!other.append(bytes_.begin(), bytes_.length())) {
    return false;
  }
  bytes_.clear();
  return true;
}

// ---- conditions

Assembler::Condition Assembler::SwapCondition(Condition cond) {
  switch (cond) {
    case Equal:
    case NotEqual:
      return cond;
    case Above:
      return Below;
    case AboveOrEqual:
      return BelowOrEqual;
    case Below:
      return Above;
    case BelowOrEqual:
      return AboveOrEqual;
    case GreaterThan:
      return LessThan;
    case GreaterThanOrEqual:
      return LessThanOrEqual;
    case LessThan:
      return GreaterThan;
    case LessThanOrEqual:
      return GreaterThanOrEqual;
    default:
      MOZ_CRASH("no swapped condition");
  }
}

Assembler::DoubleCondition Assembler::SwapCondition(DoubleCondition cond) {
  switch (cond) {
    case DoubleOrdered:
    case DoubleEqual:
    case DoubleNotEqual:
    case DoubleUnordered:
    case DoubleEqualOrUnordered:
    case DoubleNotEqualOrUnordered:
      return cond;
    case DoubleGreaterThan:
      return DoubleLessThan;
    case DoubleGreaterThanOrEqual:
      return DoubleLessThanOrEqual;
    case DoubleLessThan:
      return DoubleGreaterThan;
    case DoubleLessThanOrEqual:
      return DoubleGreaterThanOrEqual;
    case DoubleGreaterThanOrUnordered:
      return DoubleLessThanOrUnordered;
    case DoubleGreaterThanOrEqualOrUnordered:
      return DoubleLessThanOrEqualOrUnordered;
    case DoubleLessThanOrUnordered:
      return DoubleGreaterThanOrUnordered;
    case DoubleLessThanOrEqualOrUnordered:
      return DoubleGreaterThanOrEqualOrUnordered;
  }
  MOZ_CRASH("unexpected condition");
}

Assembler::Condition Assembler::UnsignedCondition(Condition cond) {
  switch (cond) {
    case GreaterThan:
      return Above;
    case GreaterThanOrEqual:
      return AboveOrEqual;
    case LessThan:
      return Below;
    case LessThanOrEqual:
      return BelowOrEqual;
    default:
      return cond;
  }
}

Assembler::Condition Assembler::ConditionWithoutEqual(Condition cond) {
  switch (cond) {
    case LessThan:
    case LessThanOrEqual:
      return LessThan;
    case Below:
    case BelowOrEqual:
      return Below;
    case GreaterThan:
    case GreaterThanOrEqual:
      return GreaterThan;
    case Above:
    case AboveOrEqual:
      return Above;
    default:
      MOZ_CRASH("unexpected condition");
  }
}

// ---- alignment

void Assembler::haltingAlign(int alignment) {
  MOZ_ASSERT(alignment % 2 == 0);
  while (currentOffset() % alignment) {
    emit(enc::illegal());
  }
}

void Assembler::nopAlign(int alignment) {
  MOZ_ASSERT(alignment % 2 == 0);
  while (currentOffset() % alignment) {
    emit(enc::nop());
  }
}

// ---- labels
//
// An unbound label keeps the source offset of its last use; the 32-bit field of each use holds the
// one before it, -1 ending the chain. A branch's displacement counts from the field itself, an
// LEA (bd32,PC)'s from its extension word two bytes before.

static constexpr int32_t ChainEnd = -1;
static constexpr uint16_t PcFullExt = 0x0170;  // the full extension word of (bd32,PC)

static int32_t PcBase(const uint8_t* code, uint32_t src) {
  return ReadCode16(code + src - 6) == PcFullExt ? int32_t(src - 6) : int32_t(src - 4);
}

static void LinkUse(uint8_t* code, uint32_t src, uint32_t target) {
  WriteCode32(code + src - 4, uint32_t(int32_t(target) - PcBase(code, src)));
}

static void ChainUse(Label* label, uint8_t* code, uint32_t src) {
  int32_t prev = label->used() ? int32_t(label->offset()) : ChainEnd;
  WriteCode32(code + src - 4, uint32_t(prev));
  label->use(src);
}

void Assembler::bind(Label* label, BufferOffset target) {
  if (label->used() && !oom()) {
    uint8_t* code = m_buffer.data();
    int32_t src = label->offset();
    while (src != ChainEnd) {
      MOZ_RELEASE_ASSERT(uint32_t(src) <= size(), "m68k64: bogus label chain");
      int32_t next = int32_t(ReadCode32(code + src - 4));
      LinkUse(code, uint32_t(src), target.getOffset());
      src = next;
    }
  }
  label->bind(target.getOffset());
}

void Assembler::retarget(Label* label, Label* target) {
  if (!label->used() || oom()) {
    label->reset();
    return;
  }
  uint8_t* code = m_buffer.data();
  int32_t src = label->offset();
  while (src != ChainEnd) {
    int32_t next = int32_t(ReadCode32(code + src - 4));
    if (target->bound()) {
      LinkUse(code, uint32_t(src), target->offset());
    } else {
      ChainUse(target, code, uint32_t(src));
    }
    src = next;
  }
  label->reset();
}

void Assembler::Bind(uint8_t* rawCode, const CodeLabel& label) {
  if (label.patchAt().bound()) {
    SetPointer(rawCode + label.patchAt().offset(), rawCode + label.target().offset());
  }
}

void Assembler::processCodeLabels(uint8_t* rawCode) {
  for (const CodeLabel& label : codeLabels_) {
    Bind(rawCode, label);
  }
}

uint32_t Assembler::jSrc(Condition cond, Label* label) {
  emit(enc::branch32(enc::Cond(cond), 0));
  uint32_t src = currentOffset();
  if (oom()) {
    return src;
  }
  if (label->bound()) {
    LinkUse(m_buffer.data(), src, label->offset());
  } else {
    ChainUse(label, m_buffer.data(), src);
  }
  return src;
}

uint32_t Assembler::fjSrc(DoubleCondition cond, Label* label) {
  emit(enc::fbranch32(enc::FCond(cond), 0));
  uint32_t src = currentOffset();
  if (oom()) {
    return src;
  }
  if (label->bound()) {
    LinkUse(m_buffer.data(), src, label->offset());
  } else {
    ChainUse(label, m_buffer.data(), src);
  }
  return src;
}

uint32_t Assembler::callSrc(Label* label) {
  emit(enc::branch32(enc::CC_F, 0));  // BSR.L
  uint32_t src = currentOffset();
  if (oom()) {
    return src;
  }
  if (label->bound()) {
    LinkUse(m_buffer.data(), src, label->offset());
  } else {
    ChainUse(label, m_buffer.data(), src);
  }
  return src;
}

void Assembler::leaLabel(Label* label, Register an) {
  MOZ_ASSERT(IsAddr(an));
  unsigned n = an.code() & 15;
  if (n >= 8) {
    emit16(uint16_t(0xA000 | enc::PFX_R));
  }
  emit16(uint16_t(0x41C0 | (n & 7) << 9 | 0x3B));  // LEA (bd32,PC),An
  emit16(PcFullExt);
  emit32(0);
  uint32_t src = currentOffset();
  if (oom()) {
    return;
  }
  if (label->bound()) {
    LinkUse(m_buffer.data(), src, label->offset());
  } else {
    ChainUse(label, m_buffer.data(), src);
  }
}

// ---- jumps and calls to other code

void Assembler::addPendingJump(uint32_t src, ImmPtr target, RelocationKind reloc) {
  MOZ_ASSERT(target.value != nullptr);
  if (reloc == RelocationKind::JITCODE) {
    jumpRelocations_.writeUnsigned(src);
  }
  static_assert(MaxCodeBytesPerProcess <= uint64_t(2) * 1024 * 1024 * 1024,
                "cross-JitCode jumps use 32-bit displacements");
  enoughMemory_ &= jumps_.append(RelativePatch(int32_t(src), target.value, reloc));
}

void Assembler::jumpTo(ImmPtr target, RelocationKind reloc) {
  if (reloc == RelocationKind::JITCODE || AddressIsInExecutableMemory(target.value)) {
    emit(enc::branch32(enc::CC_T, 0));
    addPendingJump(currentOffset(), target, reloc);
    return;
  }
  emit(enc::moveImm(uint64_t(uintptr_t(target.value)), R(ScratchAddrReg)));
  emit(enc::jmp(enc::Ea::ind(R(ScratchAddrReg))));
}

void Assembler::callTo(ImmPtr target, RelocationKind reloc) {
  if (reloc == RelocationKind::JITCODE || AddressIsInExecutableMemory(target.value)) {
    emit(enc::branch32(enc::CC_F, 0));
    addPendingJump(currentOffset(), target, reloc);
    return;
  }
  emit(enc::moveImm(uint64_t(uintptr_t(target.value)), R(ScratchAddrReg)));
  emit(enc::jsr(enc::Ea::ind(R(ScratchAddrReg))));
}

void Assembler::PatchWrite_NearCall(CodeLocationLabel start, CodeLocationLabel toCall) {
  uint8_t* p = start.raw();
  WriteCode16(p, ToggleCallOp);
  SetRel32(p + enc::kBranch32Bytes, toCall.raw());
}

// ---- finishing and copying

void Assembler::finish() {
  if (oom()) {
    return;
  }
  // Code may be followed by data: stop a fall-through.
  emit(enc::illegal());
}

void Assembler::executableCopy(void* buffer) {
  uint8_t* code = static_cast<uint8_t*>(buffer);
  memcpy(code, m_buffer.buffer(), m_buffer.size());
  for (const RelativePatch& rp : jumps_) {
    SetRel32(code + rp.offset, rp.target);
  }
}

void Assembler::copyJumpRelocationTable(uint8_t* dest) {
  if (jumpRelocations_.length()) {
    memcpy(dest, jumpRelocations_.buffer(), jumpRelocations_.length());
  }
}

void Assembler::copyDataRelocationTable(uint8_t* dest) {
  if (dataRelocations_.length()) {
    memcpy(dest, dataRelocations_.buffer(), dataRelocations_.length());
  }
}

void Assembler::TraceJumpRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader) {
  while (reader.more()) {
    uint32_t offset = reader.readUnsigned();
    uint8_t* target = GetRel32Target(code->raw() + offset);
    JitCode* child = JitCode::FromExecutable(target);
    TraceManuallyBarrieredEdge(trc, &child, "rel32");
    MOZ_ASSERT(child == JitCode::FromExecutable(GetRel32Target(code->raw() + offset)));
  }
}

void Assembler::TraceDataRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader) {
  Maybe<AutoWritableJitCode> awjc;

  while (reader.more()) {
    size_t offset = reader.readUnsigned();
    MOZ_ASSERT(offset >= sizeof(void*) && offset <= code->instructionsSize());

    uint8_t* src = code->raw() + offset;
    uintptr_t word = GetPointer(src);

    // A Value with a non-zero tag, or a raw pointer (or a zero-tagged Value).
    if (word >> JSVAL_TAG_SHIFT) {
      Value value = Value::fromRawBits(word);
      MOZ_ASSERT_IF(value.isGCThing(), gc::IsCellPointerValid(value.toGCThing()));
      TraceManuallyBarrieredEdge(trc, &value, "jit-masm-value");
      if (word != value.asRawBits()) {
        if (awjc.isNothing()) {
          awjc.emplace(code);
        }
        SetPointer(src, value.bitsAsPunboxPointer());
      }
      continue;
    }

    gc::Cell* cell = reinterpret_cast<gc::Cell*>(word);
    MOZ_ASSERT(gc::IsCellPointerValid(cell));
    TraceManuallyBarrieredGenericPointerEdge(trc, &cell, "jit-masm-ptr");
    if (uintptr_t(cell) != word) {
      if (awjc.isNothing()) {
        awjc.emplace(code);
      }
      SetPointer(src, cell);
    }
  }
}

// ---- effective addresses

enc::Ea Assembler::ea(const BaseIndex& a) {
  using enc::Ea;
  if (IsAddr(a.base)) {
    return Ea::indexed(R(a.base), R(a.index), unsigned(a.scale), a.offset);
  }
  if (a.scale == TimesOne && IsAddr(a.index)) {
    return Ea::indexed(R(a.index), R(a.base), 0, a.offset);
  }
  // LEA (0,ZA,Dbase.L),A15: the base into an address register
  MOZ_ASSERT(a.index != ScratchAddrReg);
  emit(enc::lea(Ea::indexed(0xFF, R(a.base), 0, 0), R(ScratchAddrReg)));
  return Ea::indexed(R(ScratchAddrReg), R(a.index), unsigned(a.scale), a.offset);
}

enc::Ea Assembler::ea(const Operand& op) {
  switch (op.kind()) {
    case Operand::REG:
      return ea(op.reg());
    case Operand::MEM_REG_DISP:
      return ea(op.toAddress());
    case Operand::MEM_SCALE:
      return ea(op.toBaseIndex());
    case Operand::FREG:
      break;
  }
  MOZ_CRASH("m68k64: no effective address for this operand");
}
