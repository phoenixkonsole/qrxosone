# A SpiderMonkey JIT for the 68100EC64 (design, draft 1)

ThunderCat runs JavaScript in SpiderMonkey's C++ interpreter on QRX OS One 64. This is the plan for a
code generator for the 68100EC64's long mode (`m68k64`), so that SpiderMonkey's Baseline JIT and
later Ion (Warp) emit native code. On the emulator that code is translated once more by the engine's
JIT, which works without help: the engine notices writes into code it translated (`code_map`) and
translates again. On an FPGA 68100EC64 it runs as it is.

## Why not the Portable Baseline Interpreter

SpiderMonkey has a baseline tier without code generation (`--enable-portable-baseline-interp`). With
the big-endian fix of the stub fields (`CacheIRCompiler.h`, `getStubRawInt32`) it runs on m68k64, but
it is slower than the interpreter here: 5800 ms against 3480 ms for the same JS benchmark in
ThunderCat, property access 4100 against 2260 ms (2026-11-28). It interprets each CacheIR stub through
a large switch and calls; on an emulated CPU every call and indirect jump is expensive. Only generated
code removes that work.

## Stages

| Stage | What runs natively | Rough size | Gain expected |
| --- | --- | --- | --- |
| 1 | Assembler and MacroAssembler for the integer side, trampolines, Baseline Interpreter (generated) | 8 000 lines | The interpreter's dispatch and the ICs as code |
| 2 | Baseline JIT and CacheIR stubs | +3 000 | Property access, calls and loops as code: most of what pages run |
| 3 | Ion/Warp: Lowering, CodeGenerator, MoveEmitter | +8 000 | Hot loops, arithmetic, inlining |
| later | WebAssembly | | Not before Ion |

The files follow the other 64-bit backends (`js/src/jit/loong64`, about 19 000 lines without its
simulator): `Architecture-m68k64.{h,cpp}`, `Assembler-m68k64.{h,cpp}`, `MacroAssembler-m68k64{,-inl}.h`,
`MacroAssembler-m68k64.cpp`, `MoveEmitter-m68k64.*`, `SharedICHelpers-m68k64*.h`,
`SharedICRegisters-m68k64.h`, `Trampoline-m68k64.cpp`, later `Lowering-m68k64.*`, `CodeGenerator-m68k64.*`,
`LIR-m68k64.h`. `JS_CODEGEN_M68K64` in `js/moz.configure`, no simulator (the emulator is one).

## Registers

The 68100EC64 has D0 to D15 and A0 to A15 in long mode. The C ABI (version 0, `tools/armithlon/ec64-llvm`)
keeps D2 to D7 and A2 to A6 across calls; compiled code does not use D8 to D15 and A8 to A15 at all yet,
and AROS saves all 32 registers on a task switch (`struct ExceptionContext`). A8 holds the core's data
pointer and stays untouched.

| Use | Register |
| --- | --- |
| Stack pointer | A7 |
| Frame pointer | A6 (as the C code's frame pointer, so unwinding and QRX_BACKTRACE see JIT frames) |
| JSReturnOperand (a boxed Value) | D0 |
| Baseline frame / IC stub / IC tail call | A5, A4, A3 |
| Scratch registers of the MacroAssembler | D15, A15 (and D14 for a second one) |
| Allocatable integer registers | D1 to D13 (data); A0 to A3, A9 to A14 (pointers) |

The data and address registers differ: arithmetic goes to D registers, addressing modes take A
registers as bases and D or A as index. SpiderMonkey's register allocator knows one class of general
registers; the MacroAssembler copies between the two kinds where an instruction needs the other, and
Ion's register hints prefer A registers for pointers. A Value lives in one register (`JS_PUNBOX64`).

## Big endian

SpiderMonkey's JIT assumes little endian throughout: `js/src/jit` does not mention byte order once. A
boxed Value is a 64-bit word, so loads and stores of whole Values are the same on big endian. What
changes is every access to half of a word:

- the int32 payload of a Value in memory is at offset 4, not 0 (`unboxInt32(Address)`, `load32` of
  Value slots, the boolean and the tag compare in `branchTestInt32(Address)` and the like);
- stub fields of 32 bits are kept in words (`RawInt32`: read the word, as `getStubRawInt32` now does);
- typed arrays, DataView and Wasm memory are little endian in JS, so loads and stores of their
  elements swap bytes (`load32SwapBytes` exists for big-endian DataView already);
- any `Address(base, offsetof(...))` that reads a 32-bit field out of a 64-bit one.

The MacroAssembler takes care of the Value payload: `ToPayload(Address)` adds 4 on m68k64, as
NUNBOX32 platforms already split tag and payload. The shared code (`CacheIRCompiler.cpp`,
`BaselineCodeGen.cpp`, `CodeGenerator.cpp`) is audited for raw `load32`/`store32` on 64-bit fields; each
fix is small, but missing one gives wrong values, not a crash. The js shell's jit-tests run in the
guest for that (stage 1 on, all of them with `--baseline-eager`).

## Code

- **Branches:** `Bcc.L` with a 32-bit displacement (68020 form) for all labels; short forms are an
  optimisation for later. Patchable jumps and calls: `JMP (d32,PC)`/`JSR (d32,PC)` within 2 GB, which
  holds for the JIT's own memory; calls to C++ (VM functions, ABI calls) use `MOVE.Q #imm64, A15` and
  `JSR (A15)`, patchable by rewriting the immediate.
- **Calls into C++:** the m68k64 C ABI passes all arguments on the stack in 8-byte slots, right to left;
  results in D0 (and D1), pointers in A0. `ABIArgGenerator` only assigns stack slots, which is simple.
  Results of pointer type come back in A0 and are copied to D0 where SpiderMonkey wants a GPR result.
- **Flags:** compare-and-branch maps directly (signed and unsigned conditions as on x86). `MOVE` sets
  flags; the prefix's N bit keeps them where a move must sit between a compare and its branch.
- **Constants:** 64-bit immediates under the W prefix (`MOVE.Q #imm64`); pointers to GC things are
  recorded for tracing as on x64 (`writeDataRelocation`).
- **Executable memory:** AROS has no memory protection, so `ExecutableAllocator` takes memory from
  AROS and does not need W^X switching; on the emulator the engine notices the writes. The cache flush
  (`FlushICache`) becomes `CPUSHA` (the 68040's instruction), which the engine treats as an
  invalidation of the range and an FPGA core as what it is.

## Floating point

JavaScript numbers are doubles, and the Baseline ICs already use floating-point registers (`DoubleAdd`
and the other arithmetic stubs, number comparisons, conversions); Ion more so. The 68100EC64 has no FPU.
Two ways:

1. **An FPU in the ISA.** A double-precision FPU like ColdFire's: FP0 to FP7, FMOVE.D, FADD, FSUB, FMUL,
   FDIV, FSQRT, FCMP with FBcc, FINT/FINTRZ, conversions to and from 32- and 64-bit integers. The engine
   runs it with the host's SSE or NEON (the JIT translates each one to one or two host instructions),
   an FPGA core implements it, and the SpiderMonkey backend uses it like any other. C code keeps using
   soft float and `$AFnn` until LLVM learns the FPU too, which is a separate step.
2. **No FPU.** Floating-point registers become slots in memory, and each operation a call to the
   `$AFnn` routines (D0, D1 in, D0 out). Possible, but every double operation costs a call and
   register shuffling, Ion's register allocation of floats has nothing to work with, and an FPGA
   68100EC64 has no host to hand `$AFnn` to.

Decided (2026-11-28): 1. The FPU is in the ISA ("FPU" in [68100ec64.md](68100ec64.md)) and the
reference interpreter; the engine's JITs run its instructions through the interpreter until they
translate them, and AROS saves FP0-FP7, FPCR and FPSR with a task. The backend uses FP0-FP7 as its
float registers, with FP7 as the scratch register.

## Progress

- The encoder (`tools/armithlon/spidermonkey/jit-m68k64/Encoding-m68k64.h`, no SpiderMonkey types):
  moves and constants, every addressing mode it makes (with D8-D15/A8-A15 through the prefix), the
  integer ALU, shifts, 64-bit multiply and divide, branches with 32-bit displacements, JSR/RTS,
  LINK/UNLK, CAS, bit operations, and the FPU. `tests/ec64_encoding_tests.cpp` in the engine runs what it
  encodes on the reference interpreter: 4939 checks. It found that the interpreter never reached
  EXTB.L (0x49C0 fits LEA's pattern, which came first); since the fix the 68040 comparison against
  Musashi covers those 54 more instructions, with no difference.

## Testing

- The assembler: a table of instructions with their encodings, checked against `llvm-mc` and the
  engine's interpreter (`tests/ec64_jit_tests.c` runs code both ways already).
- SpiderMonkey's `jit-tests` in the js shell in the guest, first with `--no-ion`, then
  `--baseline-eager`, then Ion; the debug build of the shell (`--enable-debug`) keeps its assertions.
- ThunderCat: the JS benchmark page (`Web/jsbench`) and qrx.rocks, against the interpreter's 3480 ms.
