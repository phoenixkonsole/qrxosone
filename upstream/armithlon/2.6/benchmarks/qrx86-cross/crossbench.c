/* ARMITHLON / QRX86 reproducible integer benchmark, MIT-licensed.
 * Same freestanding C algorithm for Linux i386 ELF and Windows i386 PE32.
 * Do not compare host GHz or unrelated Geekbench scores with this workload.
 */
#include <stdint.h>
#include <stddef.h>
#ifndef QRX_BENCH_ITERATIONS
#define QRX_BENCH_ITERATIONS 300000u
#endif
static volatile uint32_t seed = 0x13579bdfu;
static uint8_t scratch[32768];
static uint32_t rot(uint32_t x, unsigned n) {return (x << n) | (x >> (32u-n));}
static uint32_t checksum(void) {
    uint32_t x = seed;
    for(uint32_t i=0; i<sizeof scratch; ++i) scratch[i] = (uint8_t)(i ^ (i >> 5));
    for(uint32_t i=0;i<QRX_BENCH_ITERATIONS;i++) {
        x=rot(x ^ (i * 0x9e3779b9u), 7) + 0x6d2b79f5u;
        uint32_t k=(i*13u)&(sizeof(scratch)-1u);
        x ^= (uint32_t)scratch[k] << ((i&3u)*8u);
        scratch[k]=(uint8_t)(scratch[k]+(uint8_t)(x^(x>>11)));
    }
    seed = x;
    return x;
}
static char line[150];
static unsigned pos;
static void str(const char *s){while(*s&&pos<sizeof(line)-1)line[pos++]=*s++;}
static void dec(uint32_t x){char a[12];unsigned n=0;do{a[n++]=(char)('0'+x%10u);x/=10u;}while(x);while(n)line[pos++]=a[--n];}
static void hex(uint32_t x){static const char d[]="0123456789abcdef";for(int i=7;i>=0;i--)line[pos++]=d[(x>>(i*4))&15u];}
#if defined(QRX_BENCH_HOST)
#include <stdio.h>
int main(void){uint32_t x=checksum();str("QRXBENCH v=1 loops=");dec(QRX_BENCH_ITERATIONS);str(" checksum=");hex(x);str("\n");line[pos]=0;fputs(line,stdout);return 0;}
#elif defined(_WIN32)
__declspec(dllimport) void * __attribute__((stdcall)) GetStdHandle(unsigned long);
__declspec(dllimport) int __attribute__((stdcall)) WriteFile(void*,const void*,unsigned long,unsigned long*,void*);
__declspec(dllimport) void __attribute__((stdcall)) ExitProcess(unsigned);
void mainCRTStartup(void){unsigned long written=0;uint32_t x=checksum();str("QRXBENCH v=1 loops=");dec(QRX_BENCH_ITERATIONS);str(" checksum=");hex(x);str("\n");WriteFile(GetStdHandle((unsigned long)-11),line,pos,&written,0);ExitProcess(0);}
#else
static int linux_write(int fd,const void *buf,unsigned n){int ret; __asm__ volatile("int $0x80":"=a"(ret):"a"(4),"b"(fd),"c"(buf),"d"(n):"memory","cc");return ret;}
static void linux_exit(unsigned status){__asm__ volatile("int $0x80"::"a"(1),"b"(status):"memory","cc");for(;;) {}}
void _start(void){uint32_t x=checksum();str("QRXBENCH v=1 loops=");dec(QRX_BENCH_ITERATIONS);str(" checksum=");hex(x);str("\n");int n=linux_write(1,line,pos);linux_exit(n==(int)pos?0:91);}
#endif
