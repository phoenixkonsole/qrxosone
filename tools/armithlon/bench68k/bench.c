/* Bare-metal CPU workload for the QRX68100 benchmark: fixed-point math, memory
 * copies, sieve, string hashing and sorting, the mix a Doom-style renderer and
 * the QRX86 translator produce. No OS; ends by writing the shutdown register. */
typedef unsigned int u32; typedef int s32; typedef unsigned char u8; typedef unsigned short u16;
#define HALT (*(volatile u32*)0xFF00001Cu)
static u32 do_fixed(u32 n);static u32 do_sieve(void);static u32 do_copy(void);static u32 do_sort(void);static u32 do_hash(void);
/* First in the file: the HUNK loader enters at offset 0 (-fno-toplevel-reorder). */
void _start(void){
    volatile u32 result=0;
    for(u32 round=0;round<8;round++){
        result+=do_fixed(200000);
        result+=do_sieve();
        result+=do_copy();
        result+=do_sort();
        result+=do_hash();
    }
    *(volatile u32*)0x8000u=result;
    HALT=1;
    for(;;){}
}
static u8 sieve[65536];
static u32 table[4096];
static u8 src[32768],dst[32768];
static inline s32 fixmul(s32 a,s32 b){long long r=(long long)a*b;return (s32)(r>>16);}
static u32 do_fixed(u32 n){
    s32 x=0x12345,y=0x7FFF1,sum=0;
    for(u32 i=0;i<n;i++){x=fixmul(x,0x10203)+y;y=(y^(x>>3))+i;sum+=x>>5;x&=0xFFFFFF;}
    return (u32)sum;
}
static u32 do_sieve(void){
    u32 c=0;
    for(u32 i=0;i<sizeof sieve;i++)sieve[i]=1;
    for(u32 i=2;i<sizeof sieve;i++)if(sieve[i]){c++;for(u32 j=i+i;j<sizeof sieve;j+=i)sieve[j]=0;}
    return c;
}
static u32 do_copy(void){
    u32 h=0;
    for(u32 i=0;i<sizeof src;i++)src[i]=(u8)(i*31u);
    for(u32 r=0;r<16;r++){
        const u8 *s=src;u8 *d=dst;
        for(u32 i=0;i<sizeof src/4;i++){*(u32*)d=*(const u32*)s;d+=4;s+=4;}
        h+=dst[r*17u];
    }
    return h;
}
static u32 do_sort(void){
    u32 v=12345;
    for(u32 i=0;i<4096;i++){v=v*1103515245u+12345u;table[i]=v;}
    for(s32 gap=2048;gap>0;gap/=2)for(u32 i=(u32)gap;i<4096;i++){
        u32 t=table[i];s32 j=(s32)i;
        while(j>=gap&&table[j-gap]>t){table[j]=table[j-gap];j-=gap;}
        table[j]=t;
    }
    return table[100];
}
static u32 do_hash(void){
    static const char text[]="QRX OS One ARMithlon 68100 benchmark text for hashing loops";
    u32 h=2166136261u;
    for(u32 r=0;r<4000;r++)for(const char *p=text;*p;p++)h=(h^(u8)*p)*16777619u;
    return h;
}
