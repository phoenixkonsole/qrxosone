/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/m68k64/Architecture-m68k64.h"

#include <string.h>

#include "jit/FlushICache.h"  // js::jit::FlushICache
#include "jit/RegisterSets.h"

namespace js {
namespace jit {

Registers::Code Registers::FromName(const char* name) {
  for (size_t i = 0; i < Total; i++) {
    if (strcmp(GetName(i), name) == 0) {
      return Code(i);
    }
  }
  return Invalid;
}

FloatRegisters::Code FloatRegisters::FromName(const char* name) {
  for (size_t i = 0; i < TotalPhys; i++) {
    if (strcmp(GetName(i), name) == 0) {
      return Code(i);  // the double register
    }
  }
  return Invalid;
}

// Singles and doubles are the same eight registers: push each once, as a double.
FloatRegisterSet FloatRegister::ReduceSetForPush(const FloatRegisterSet& s) {
  LiveFloatRegisterSet ret;
  for (FloatRegisterIterator iter(s); iter.more(); ++iter) {
    ret.addUnchecked(FromCode((*iter).encoding()));
  }
  return ret.set();
}

uint32_t FloatRegister::GetPushSizeInBytes(const FloatRegisterSet& s) {
  return s.size() * sizeof(double);
}

uint32_t FloatRegister::getRegisterDumpOffsetInBytes() { return encoding() * sizeof(double); }

bool CPUFlagsHaveBeenComputed() { return true; }

// The emulator notices writes into code it translated and translates it again, so nothing is
// needed there. An FPGA 68100EC64 will need exec's CacheClearE() here.
void FlushICache(void* code, size_t size) {}

}  // namespace jit
}  // namespace js
