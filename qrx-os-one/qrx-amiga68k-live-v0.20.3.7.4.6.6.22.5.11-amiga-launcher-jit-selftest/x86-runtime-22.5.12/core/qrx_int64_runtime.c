/* Portable compiler runtime for generic m68k ELF GCC on a 68060.
 * Packaged Linux libgcc may emit 68020 wide MUL/DIV; these routines use only
 * word-sized shifts/add/subtract. No guest instruction semantics live here. */
#include <stdint.h>
#include <stdlib.h>
static uint64_t qrx_udiv64(uint64_t a,uint64_t b,uint64_t *rem)
{
    uint64_t q=0,r=0;
    if(!b)abort();
    for(unsigned i=64;i;i--){unsigned carry=r>>63;r=(r<<1)|((a>>(i-1))&1);
        if(carry||r>=b){r-=b;q|=(uint64_t)1<<(i-1);}}
    if(rem)*rem=r;
    return q;
}
uint64_t __udivdi3(uint64_t a,uint64_t b){return qrx_udiv64(a,b,NULL);}
uint64_t __umoddi3(uint64_t a,uint64_t b){uint64_t r;qrx_udiv64(a,b,&r);return r;}
uint64_t __udivmoddi4(uint64_t a,uint64_t b,uint64_t *r){return qrx_udiv64(a,b,r);}
int64_t __divdi3(int64_t a,int64_t b)
{
    uint64_t ua=a,ub=b;if(a<0)ua=0-ua;if(b<0)ub=0-ub;
    uint64_t q=qrx_udiv64(ua,ub,NULL);return (int64_t)(((a<0)!=(b<0))?0-q:q);
}
int64_t __moddi3(int64_t a,int64_t b)
{
    uint64_t ua=a,ub=b,r;if(a<0)ua=0-ua;if(b<0)ub=0-ub;
    qrx_udiv64(ua,ub,&r);return (int64_t)(a<0?0-r:r);
}
uint64_t __muldi3(uint64_t a,uint64_t b)
{
    /* Keep the conditional accumulation word-sized: GCC may otherwise turn
     * if(bit) r+=a into a 64-bit multiply and recursively call this helper. */
    uint32_t al=(uint32_t)a,ah=(uint32_t)(a>>32),bl=(uint32_t)b,bh=(uint32_t)(b>>32);
    uint32_t lo=0,hi=0;
    while(bl||bh){
        if(bl&1){uint32_t old=lo;lo+=al;hi+=ah+(lo<old);}
        ah=(ah<<1)|(al>>31);al<<=1;bl=(bl>>1)|(bh<<31);bh>>=1;
    }
    return ((uint64_t)hi<<32)|lo;
}
