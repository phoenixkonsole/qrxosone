/* Gate the experimental generic-ELF-to-AROS adapter before using a JIT binary.
 * Uses the same private-stack wrapper and CRT pointer-return surfaces as QRX. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qrx_bigstack.h"
static int probe_main(int argc,char **argv)
{
    char name[80],line[32],*saved=NULL,*word;
    (void)argc;(void)argv;
    setbuf(stdout,NULL);
    char *p=calloc(128,1);
    if(!p)return 20;
    for(unsigned i=0;i<128;i++)if(((volatile unsigned char*)p)[i])return 21;
    p=realloc(p,256);
    if(!p)return 22;
    strcpy(p,"one,two");
    word=strtok_r(p,",",&saved);
    if(!word||strcmp(word,"one"))return 23;
    word=strtok_r(NULL,",",&saved);
    if(!word||strcmp(word,"two"))return 24;
    char *copy=strdup(word);
    if(!copy||strcmp(copy,"two"))return 25;
    snprintf(name,sizeof name,"T:qrx-sdk-probe-%p.tmp",(void*)p);
    FILE *f=fopen(name,"w+");
    if(!f)return 26;
    fputs("stdio-pointer-bridge\n",f);
    if(fseek(f,0,SEEK_SET)||!fgets(line,sizeof line,f)||strcmp(line,"stdio-pointer-bridge\n"))return 27;
    if(fclose(f))return 28;
    remove(name);free(copy);free(p);
    puts("QRX SDK ABI PASS (private stack, allocation, strings, FILE pointers)");
    return 0;
}
QRX_BIGSTACK_MAIN(probe_main)
