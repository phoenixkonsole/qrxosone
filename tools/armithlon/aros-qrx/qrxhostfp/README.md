# qrxhostfp: soft float as the 68100EC64's host floating point instruction

The 68100EC64 has no FPU. Every `float` or `double` operation in m68k64 code is a call to
compiler-rt's soft float (`__addsf3`, `__mulsf3`, `__fixsfsi`, ...), 50 to 200 instructions each.
The engine has an instruction for this (`$AFnn`, see
[docs/armithlon/68100ec64.md](../../../../docs/armithlon/68100ec64.md#host-floating-point-afnn)):
the host does the operation and returns the same bits compiler-rt would.

| File | |
| --- | --- |
| `make-hostfp.py` | writes `hostfp.S` and `hostfp.inc`: 42 functions, each loads its arguments from the stack into D0 and D1, runs `$AFnn` and returns |
| `hostfp.S` | for programs: link `hostfp.o` before the libraries, its definitions win |
| `hostfp.inc` | the same as top-level asm for a C file (ThunderCat: `widget/aros/aros_hostfp.inc`, included by `aros_appshell_glue.c`) |
| `fptest.c` | prints a hash of 7354 float results; built once with and once without `hostfp.o`, both must print the same |

`__cmpsf2` and `__cmpdf2` are left out: in Rust's compiler_builtins they sit in the same object as
the other comparisons, and linking both gave duplicate symbols.

A program built with these runs only on an engine with `$AFnn` (68100EC64 from this change on);
elsewhere the first float operation is an illegal instruction.

Test (in the guest, `C:` on hostfs):

```
cc64 -O1 fptest.c -o fptest-soft -lm
cc64 -O1 fptest.c hostfp.o -o fptest-host -lm
C:fptest-soft     ; FPHASH 879075ae4968ac4a
C:fptest-host     ; the same
```
