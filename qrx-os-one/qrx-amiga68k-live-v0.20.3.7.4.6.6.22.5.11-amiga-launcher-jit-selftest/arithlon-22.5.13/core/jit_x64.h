/* Arithlon 22.5.13 - 68k -> x86-64 dynamic recompiler (see jit_x64.c) */
#ifndef ARITHLON_JIT_X64_H
#define ARITHLON_JIT_X64_H
#include "m68k.h"
#include <stdio.h>
typedef struct jit jit_t;
jit_t *jit_new(m68k *c);                              /* NULL: host not supported */
int    jit_run(jit_t *j, m68k *c);                    /* until stop or fault; returns c->fault */
void   jit_invalidate(jit_t *j, uint32_t addr, uint32_t len);   /* guest code changed / unmapped */
void   jit_sync_pc(jit_t *j, m68k *c);                /* after a host fault inside translated code */
void   jit_stats(jit_t *j, FILE *f);
#endif
