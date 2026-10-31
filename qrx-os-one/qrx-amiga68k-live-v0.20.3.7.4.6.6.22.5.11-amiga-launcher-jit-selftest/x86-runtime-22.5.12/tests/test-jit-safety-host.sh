#!/usr/bin/env bash
set -Eeuo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
WK="${1:?usage: test-jit-safety-host.sh WORKDIR}"
mkdir -p "$WK/mock/exec" "$WK/mock/proto"
WK="$(cd "$WK" && pwd)"
bash "$HERE/qmath/build-qmath.sh" cc ar "$WK" > "$WK/build.log" 2>&1
cc -std=gnu99 -O2 -Wno-unused-function "$HERE/tests/jit-differential.c" "$WK/libqmath.a" -lm -o "$WK/unsupported-host" >> "$WK/build.log" 2>&1
"$WK/unsupported-host"
cc -std=c99 -O2 -Wall -Wextra -Werror -o "$WK/legacy-helper" "$ROOT/tests-22.5.11/test-jit-helper.c"
"$WK/legacy-helper"
cat > "$WK/mock/exec/tasks.h" <<'H'
#include <stdint.h>
typedef void *APTR;typedef uintptr_t IPTR;typedef void (*LONG_FUNC)(void);
struct StackSwapStruct{void *stk_Lower,*stk_Upper,*stk_Pointer;};
struct StackSwapArgs{IPTR Args[8];};
H
printf '#define MEMF_ANY 0\n' > "$WK/mock/exec/memory.h"
cat > "$WK/mock/proto/exec.h" <<'H'
#include <stddef.h>
static void *AllocMem(unsigned n,unsigned flags){(void)n;(void)flags;return NULL;}
static void FreeMem(void *p,unsigned n){(void)p;(void)n;}
static IPTR NewStackSwap(struct StackSwapStruct *s,LONG_FUNC f,struct StackSwapArgs *a){(void)s;(void)f;(void)a;return 0;}
H
cat > "$WK/stack.c" <<'C'
#include <assert.h>
#include <string.h>
static int calls;
static int real_main(int argc,char **argv){(void)argc;(void)argv;calls++;return 0;}
#define __AROS__ 1
#include "qrx_bigstack.h"
#define main wrapped_main
QRX_BIGSTACK_MAIN(real_main)
#undef main
int main(void){assert(wrapped_main(0,0)==20&&calls==0);return 0;}
C
cc -std=gnu99 -O2 -I"$WK/mock" -I"$HERE/core" "$WK/stack.c" -o "$WK/stack"
"$WK/stack"
echo 'PASS stack allocation failure never enters large runtime on the DOS stack'
# Independent native x86_64 division validates the 68060 test-runtime helpers.
cat > "$WK/div.c" <<'C'
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
uint64_t __udivdi3(uint64_t,uint64_t);uint64_t __umoddi3(uint64_t,uint64_t);
uint64_t __muldi3(uint64_t,uint64_t);int64_t __divdi3(int64_t,int64_t);int64_t __moddi3(int64_t,int64_t);
int main(void){uint64_t x=0x123456789abcdefULL;
for(unsigned i=0;i<100000;i++){x^=x<<13;x^=x>>7;x^=x<<17;uint64_t a=x;x^=x<<13;x^=x>>7;x^=x<<17;uint64_t b=x|1;assert(__udivdi3(a,b)==a/b&&__umoddi3(a,b)==a%b&&__muldi3(a,b)==a*b);int64_t sa=(int64_t)a,sb=(int64_t)b;if(sa!=INT64_MIN||sb!=-1)assert(__divdi3(sa,sb)==sa/sb&&__moddi3(sa,sb)==sa%sb);}
void *p=calloc(8,4);assert(p);for(unsigned i=0;i<32;i++)assert(((unsigned char*)p)[i]==0);free(p);return 0;}
C
cc -O2 -fno-builtin "$WK/div.c" "$HERE/tests/m68k-linux-68060-support.c" -o "$WK/div"
"$WK/div"
echo 'PASS 68060 test-runtime division vs native x86_64: 100000 cases'
