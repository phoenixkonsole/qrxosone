#!/usr/bin/env bash
# QRX 22.5.11 - verify the self-test emitter byte-for-byte against GNU as for m68k.
# HOST-ONLY: proves the encodings; native execution is tested only on the target.
set -Eeuo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AS="${QRX_M68K_AS:-$(command -v m68k-linux-gnu-as || command -v m68k-aros-as || true)}"
OBJCOPY="${QRX_M68K_OBJCOPY:-$(command -v m68k-linux-gnu-objcopy || command -v m68k-aros-objcopy || true)}"
[[ -x "$AS" && -x "$OBJCOPY" ]] || { echo "SKIP: no m68k assembler (install binutils-m68k-linux-gnu)"; exit 77; }
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
cat > "$T/ref.s" <<'ASM'
    .text
    moveq   #42,%d0
    moveq   #-1,%d3
    move.l  #0x51525354,%d0
    move.l  #0x80000001,%d7
    addq.l  #1,%d0
    addq.l  #8,%d2
    jsr     0x12345678:l
    nop
    rts
ASM
"$AS" -m68020 -o "$T/ref.o" "$T/ref.s"
"$OBJCOPY" -O binary -j .text "$T/ref.o" "$T/ref.bin"
cat > "$T/emit.c" <<'C'
#include <stdio.h>
#include "qrx_jit_emit.h"
int main(void){unsigned char b[64];struct qrx_emit e;qe_init(&e,b,sizeof b);
qe_moveq(&e,42,0);qe_moveq(&e,-1,3);qe_move_l_imm(&e,0x51525354UL,0);qe_move_l_imm(&e,0x80000001UL,7);
qe_addq_l(&e,1,0);qe_addq_l(&e,8,2);qe_jsr_abs(&e,0x12345678UL);qe_nop(&e);qe_rts(&e);
if(e.overflow)return 3;
fwrite(b,1,e.len,stdout);
{unsigned char s[3]={0xaa,0xbb,0xcc};struct qrx_emit o;qe_init(&o,s,sizeof s);qe_w32(&o,0x11223344UL);if(!o.overflow||o.len||s[0]!=0xaa||s[1]!=0xbb||s[2]!=0xcc)return 4;qe_w16(&o,0x1234);if(o.len)return 5;}
return 0;}
C
cc -std=c99 -Wall -Wextra -Werror -Wno-unused-function -I"$HERE/../src" -o "$T/emit" "$T/emit.c"
"$T/emit" > "$T/emit.bin"
if cmp -s "$T/ref.bin" "$T/emit.bin"; then
  echo "PASS: m68k emitter matches GNU as byte-for-byte ($(stat -c %s "$T/ref.bin") bytes: $(od -An -tx1 "$T/ref.bin" | tr -d '\n' | sed 's/^ //'))"
  echo "PASS: emitter overflow guard"
else
  echo "FAIL: emitter mismatch"; od -An -tx1 "$T/ref.bin"; od -An -tx1 "$T/emit.bin"; exit 1
fi
