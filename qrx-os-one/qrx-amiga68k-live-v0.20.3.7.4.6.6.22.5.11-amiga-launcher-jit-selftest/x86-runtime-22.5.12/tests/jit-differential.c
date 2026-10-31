/* Real generated-m68k execution versus the unchanged interpreter, in one process.
 * Including the implementation also permits independent assembler byte checks.
 * This is not a claim of real-hardware or AROS ABI validation. */
#include "../core/qrx86core.c"
#include <assert.h>
#ifdef QRX_JIT_ORACLE
extern int oracle_x86_run(x86cpu *,uint32_t);
extern int oracle_x86_step(x86cpu *);
#define reference_step oracle_x86_step
#define reference_run oracle_x86_run
#else
#define reference_run x86_run
#define reference_step x86_step
#endif

#define CODE 0x10000u
#define DATA 0x20000u
static x86cpu a, b;
static uint32_t rng = 0x22512;
static unsigned check_watchdog;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void put32(uint8_t *p, uint32_t v) { p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24; }
static void init(unsigned profile)
{
    x86_init(&a);x86_init(&b);
    assert(x86_map(&a,CODE,8192,1)&&x86_map(&b,CODE,8192,1));
    assert(x86_map(&a,DATA,4096,1)&&x86_map(&b,DATA,4096,1));
    assert(!x86_jit_enable(&b, 10, 0));
    assert(x86_jit_enable(&b,profile,0));
    b.jit->min_ops=3;b.jit->allow_loads=1;b.jit->stats.cache_profile=1;b.jit->threshold=2; /* deterministic cold/warm coverage, independent of environment */
}
static void reset(x86cpu *c, const uint32_t *regs, uint32_t flags)
{ memcpy(c->r,regs,32);x86_set_flags(c,flags);c->fault=c->stop=0;c->eip=CODE;c->budget=0; }
static void check(const uint8_t *p, unsigned n, const uint32_t *regs, uint32_t flags)
{
    unsigned repeat;
    assert(x86_write_mem(&a,CODE,p,n)&&x86_write_mem(&b,CODE,p,n));
    /* A previous longer program must not extend this block beyond the stop PC. */
    {uint8_t end=0xcc;assert(x86_write_mem(&a,CODE+n,&end,1)&&x86_write_mem(&b,CODE+n,&end,1));}
    for(repeat=0;repeat<3;repeat++){
        uint64_t ai=x86_icount(&a), bi=x86_icount(&b); int fa,fb;
        reset(&a,regs,flags);reset(&b,regs,flags);
        if(check_watchdog){a.budget=ai+check_watchdog;b.budget=bi+check_watchdog;}
        fa=reference_run(&a,CODE+n);fb=x86_run(&b,CODE+n);
        if(fa!=fb||memcmp(a.r,b.r,32)||a.eip!=b.eip||x86_get_flags(&a)!=x86_get_flags(&b)||
           x86_icount(&a)-ai!=x86_icount(&b)-bi){
            fprintf(stderr,"DIFFERENTIAL FAIL rng=%08x repeat=%u len=%u eip=%x/%x faults=%d/%d\n",rng,repeat,n,a.eip,b.eip,fa,fb);
            for(unsigned i=0;i<n;i++)fprintf(stderr,"%02x",p[i]);fprintf(stderr,"\n");exit(1);
        }
        assert(!fa);
    }
}
static void extended_register_tests(void)
{
    uint8_t p[64];uint32_t regs[8];uint64_t native=b.jit->stats.native_instructions;
    /* Every ModRM/SIB shape, including ESP-no-index, no-base, aliases and wrap. */
    for(unsigned mod=0;mod<3;mod++)for(unsigned rm=0;rm<8;rm++)for(unsigned sib=0;sib<256;sib++) {
        unsigned n=0,dst=sib&7;
        for(unsigned k=0;k<8;k++)regs[k]=rnd();
        p[n++]=0x90;p[n++]=0x90;p[n++]=0x8d;p[n++]=(mod<<6)|(dst<<3)|rm;
        if(rm==4)p[n++]=sib;
        if(mod==1)p[n++]=rnd();
        else if(mod==2 || (mod==0&&(rm==5 || (rm==4&&(sib&7)==5)))){put32(p+n,rnd());n+=4;}
        p[n++]=0x89;p[n++]=0xc3;
        check(p,n,regs,rnd());
    }
    for(unsigned group=4;group<8;group++)for(unsigned count=0;count<256;count++) {
        unsigned n=0;for(unsigned k=0;k<8;k++)regs[k]=rnd();
        p[n++]=0x90;p[n++]=0x90;p[n++]=0xc1;p[n++]=0xc0|(group<<3)|(count&7);p[n++]=count;
        /* Consume flags through every long Jcc; no loops. */
        p[n++]=0x0f;p[n++]=0x80+(count&15);put32(p+n,5);n+=4;
        p[n++]=0xbb;put32(p+n,rnd());n+=4;p[n++]=0x90;
        check(p,n,regs,rnd());
    }
    assert(b.jit->stats.native_instructions>native+30000);
    puts("PASS native LEA all ModRM/SIB forms + shifts all immediate counts + near Jcc");
}
static void condition_and_step_tests(void)
{
    uint8_t p[64];uint32_t regs[8]={0};
    const unsigned ops[]={0x01,0x29,0x21,0x09,0x31,0x39,0x85};
    const uint32_t left[]={0,0xffffffff,0x7fffffff,0x80000000,1,0,0x80000000,0x55555555};
    const uint32_t right[]={0,1,1,0xffffffff,0xffffffff,1,1,0xaaaaaaaa};
    for(unsigned op=0;op<7;op++)for(unsigned edge=0;edge<8;edge++)for(unsigned cc=0;cc<16;cc++) {
        unsigned n=0;regs[0]=left[edge];regs[1]=right[edge];
        p[n++]=ops[op];p[n++]=0xc8;
        /* Clobber D0/D1 after the captured arithmetic condition. */
        p[n++]=0x8d;p[n++]=0x74;p[n++]=0x48;p[n++]=4;
        p[n++]=0x89;p[n++]=0xd8;
        p[n++]=0x70+cc;p[n++]=5;p[n++]=0xbb;put32(p+n,rnd());n+=4;p[n++]=0x90;
        check(p,n,regs,rnd());
        xj_block *block=b.jit->slots[(CODE^(CODE>>7))&(XJ_SLOTS-1)].block;
        assert(block&&block->native_cc==(cc!=10&&cc!=11));
    }
    {uint8_t code[]={0xb8,0xff,0xff,0xff,0x7f,0x40,0x05,1,0,0,0,0xcc};
     assert(x86_write_mem(&a,CODE,code,sizeof code)&&x86_write_mem(&b,CODE,code,sizeof code));
     reset(&a,regs,0x202);reset(&b,regs,0x202);
     for(unsigned i=0;i<4;i++){
        uint64_t ai=x86_icount(&a),bi=x86_icount(&b);
        int fa=reference_step(&a),fb=x86_step(&b);
        assert(fa==fb&&a.eip==b.eip&&!memcmp(a.r,b.r,32)&&x86_get_flags(&a)==x86_get_flags(&b));
        assert(x86_icount(&a)-ai==x86_icount(&b)-bi);
     }}
    puts("PASS native arithmetic conditions (all Jcc, parity fallback, post-flag clobbers) and single-step oracle");
}
static void memory_decode_tests(void)
{
    uint8_t p[32],data[8192];uint32_t regs[8];
    assert(x86_map(&a,DATA,8192,1)&&x86_map(&b,DATA,8192,1));
    for(unsigned i=0;i<sizeof data;i++)data[i]=(uint8_t)rnd();
    assert(x86_write_mem(&a,DATA,data,sizeof data)&&x86_write_mem(&b,DATA,data,sizeof data));
    for(unsigned asz=0;asz<2;asz++)for(unsigned osz=0;osz<2;osz++)for(unsigned mod=0;mod<3;mod++)for(unsigned rm=0;rm<8;rm++)for(unsigned sib=0;sib<256;sib+=17){
        unsigned n=0;
        for(unsigned k=0;k<8;k++)regs[k]=0x100;
        a.sbase[X86_DS]=b.sbase[X86_DS]=DATA;
        a.sbase[X86_SS]=b.sbase[X86_SS]=DATA+0x200;
        a.sbase[X86_FS]=b.sbase[X86_FS]=DATA+0x400;
        if(asz)p[n++]=0x67;if(osz)p[n++]=0x66;if((sib&3)==0)p[n++]=0x64;
        p[n++]=(sib&1)?0x8b:0x01;p[n++]=(mod<<6)|((sib&7)<<3)|rm;
        if(!asz && rm==4)p[n++]=sib;
        if(mod==1)p[n++]=0xe0;
        else if(mod==2 || (!mod && (asz?rm==6:rm==5||(rm==4&&(sib&7)==5)))) {
            put32(p+n,0x40);n+=asz?2:4;
        }
        p[n++]=0x90;p[n++]=0x90;check(p,n,regs,0x202);
        for(unsigned page=0;page<2;page++)assert(!memcmp(x86_host_ptr(&a,DATA+page*4096),x86_host_ptr(&b,DATA+page*4096),4096));
    }
    a.sbase[X86_DS]=b.sbase[X86_DS]=a.sbase[X86_SS]=b.sbase[X86_SS]=a.sbase[X86_FS]=b.sbase[X86_FS]=0;
    x86_unmap(&a,DATA+4096,4096);x86_unmap(&b,DATA+4096,4096);
    puts("PASS ModRM memory decoder: 16/32-bit addressing and operands, SIB, DS/SS/FS, loads and writes");
}
static void carry_tests(void)
{
    uint8_t p[128];uint32_t regs[8];
    const uint32_t edges[]={0,1,0x7fffffff,0x80000000,0xffffffff,0xfffffffe,0xf,0x10};
    uint64_t native=b.jit->stats.native_instructions;
    for(unsigned a0=0;a0<8;a0++)for(unsigned b0=0;b0<8;b0++)for(unsigned cf=0;cf<2;cf++)for(unsigned cc=0;cc<16;cc++){
        unsigned n=0;for(unsigned k=0;k<8;k++)regs[k]=rnd();regs[0]=edges[a0];regs[1]=edges[b0];
        p[n++]=0x90;p[n++]=0x40;p[n++]=0x48; /* preserves entry carry across both */
        p[n++]=0x11;p[n++]=0xc8; /* ADC reg, reg */
        p[n++]=0x83;p[n++]=0xd8;p[n++]=0xff; /* SBB imm: forces split after ADC */
        p[n++]=0x49;p[n++]=0x41;p[n++]=0x89;p[n++]=0xc2;
        p[n++]=0x0f;p[n++]=0x80+cc;put32(p+n,5);n+=4;
        p[n++]=0xbb;put32(p+n,rnd());n+=4;p[n++]=0x90;
        check(p,n,regs,0xad6|cf);
    }
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_CARRY_CHAIN
    /* Prior full flag producers must not split INC/DEC or carry arithmetic. */
    for(unsigned producer=0;producer<5;producer++)for(unsigned v=0;v<8;v++)for(unsigned cf=0;cf<2;cf++){
        unsigned n=0;memset(regs,0,sizeof regs);regs[0]=edges[v];regs[1]=edges[(v+1)%8];
        p[n++]=0x90;p[n++]=0x90;
        if(producer<4){static const uint8_t op[]={0x01,0x29,0x39,0x31};p[n++]=op[producer];p[n++]=0xc8;}
        else {p[n++]=0xc1;p[n++]=0xf8;p[n++]=16;}
        p[n++]=0x42;p[n++]=0x4a; /* INC/DEC edx preserve producer CF */
        p[n++]=0x11;p[n++]=0xc8;p[n++]=0x19;p[n++]=0xc0; /* ADC alias + SBB eax,eax */
        p[n++]=0x11;p[n++]=0xc9;p[n++]=0x90;
        check(p,n,regs,0x202|cf);
        xj_block block={0};xj_op decoded[XJ_MAX_OPS];unsigned count;int flags;
        block.eip=CODE;assert(xj_decode(&b,&block,decoded,&count,&flags));
        assert(block.end==CODE+n&&count==9);
    }
    puts("PASS unsplit native CF chains: ADD/SUB/CMP/XOR/SAR -> INC/DEC -> ADC/SBB, aliases");
#endif
    assert(b.jit->stats.native_instructions>native+10000);
    puts("PASS native INC/DEC/ADC/SBB: carry chains, signed/unsigned edges and all Jcc");
}
static void leading_load_tests(void)
{
#if QRX_JIT_NATIVE_LOADS
    uint8_t p[32],data[4096];uint32_t regs[8];
    uint64_t native=b.jit->stats.native_instructions;
    for(unsigned k=0;k<4096;k++)data[k]=(uint8_t)rnd();
    assert(x86_write_mem(&a,DATA,data,4096)&&x86_write_mem(&b,DATA,data,4096));
    for(unsigned offset=0;offset<=4092;offset+=3)for(unsigned scale=0;scale<4;scale++){
        unsigned n=0,dst=offset&7;for(unsigned k=0;k<8;k++)regs[k]=rnd();
        regs[3]=DATA;regs[1]=offset>>scale;
        p[n++]=0x8b;p[n++]=0x84|(dst<<3);p[n++]=(scale<<6)|(1<<3)|3;
        put32(p+n,offset-((offset>>scale)<<scale));n+=4;
        p[n++]=0x83;p[n++]=0xc0|dst;p[n++]=1;
        p[n++]=0x89;p[n++]=0xc0|(dst<<3)|2;
        check(p,n,regs,rnd());
        assert(b.r[2]==ld32(data+offset)+1);
    }
    if(b.jit->stats.native_instructions<=native+20000)fprintf(stderr,"LOAD COVERAGE delta=%lu allow=%d min=%u threshold=%u\n",(unsigned long)(b.jit->stats.native_instructions-native),b.jit->allow_loads,b.jit->min_ops,b.jit->threshold);
    assert(b.jit->stats.native_instructions>native+20000);
    for(unsigned base=3;base<=5;base++) {
        unsigned n=0;memset(regs,0,sizeof regs);
        a.sbase[X86_DS]=b.sbase[X86_DS]=0x8000;
        a.sbase[X86_SS]=b.sbase[X86_SS]=0x4000;
        regs[base]=DATA-(base==3?0x8000:0x4000)+3;
        p[n++]=0x8b;p[n++]=0x40|base;if(base==4)p[n++]=0x24;p[n++]=0;
        p[n++]=0x83;p[n++]=0xc0;p[n++]=1;p[n++]=0x89;p[n++]=0xc2;
        check(p,n,regs,0x202);assert(b.r[2]==ld32(data+3)+1);
    }
    a.sbase[X86_DS]=b.sbase[X86_DS]=a.sbase[X86_SS]=b.sbase[X86_SS]=0;
    /* Reuse one block as data mappings and offsets change, including faults. */
    {uint8_t q[]={0x8b,0x03,0x83,0xc0,1,0x89,0xc2};
     for(unsigned off=4092;off<=4096;off++)for(unsigned repeat=0;repeat<3;repeat++){
        memset(regs,0,sizeof regs);regs[3]=DATA+off;
        assert(x86_write_mem(&a,CODE,q,sizeof q)&&x86_write_mem(&b,CODE,q,sizeof q));
        reset(&a,regs,0xa97);reset(&b,regs,0xa97);
        uint64_t ai=x86_icount(&a),bi=x86_icount(&b);
        int fa=reference_run(&a,CODE+sizeof q),fb=x86_run(&b,CODE+sizeof q);
        assert(fa==fb&&!memcmp(a.r,b.r,32)&&x86_get_flags(&a)==x86_get_flags(&b));
        assert(x86_icount(&a)-ai==x86_icount(&b)-bi);
        if(off>4092)assert(fa==X86_FAULT_PAGE&&a.fault_eip==b.fault_eip&&a.fault_addr==b.fault_addr);
     }
     for(unsigned pass=0;pass<3;pass++){
        x86_unmap(&a,DATA,4096);x86_unmap(&b,DATA,4096);
        assert(x86_map(&a,DATA,4096,1)&&x86_map(&b,DATA,4096,1));
        data[0]++;assert(x86_write_mem(&a,DATA,data,4096)&&x86_write_mem(&b,DATA,data,4096));
        x86_protect(&a,DATA,4096,0);x86_protect(&b,DATA,4096,0);
        memset(regs,0,sizeof regs);regs[3]=DATA;check(q,sizeof q,regs,0x202);
        assert(b.r[2]==ld32(data)+1);
     }
    }
    puts("PASS leading native loads: LE conversion, unaligned/SIB/aliases, remap, read-only, page faults and instruction charge");
#else
    puts("SKIP optional native loads (not compiled in this build)");
#endif
}
static void random_tests(void)
{
    unsigned trial; uint8_t p[256];uint32_t regs[8];
    static const unsigned alur[]={0x01,0x03,0x09,0x0b,0x21,0x23,0x29,0x2b,0x31,0x33,0x39,0x3b,0x85,0x89,0x8b};
    static const unsigned groups[]={0,1,4,5,6,7};
    static const unsigned accum[]={0x05,0x0d,0x25,0x2d,0x35,0x3d,0xa9};
    for(trial=0;trial<2000;trial++){
        unsigned n=0,i;
        for(i=0;i<8;i++)regs[i]=rnd();
        for(i=0;i<12;i++){
            unsigned op=rnd()%6, reg=rnd()&7;
            if(op==0){p[n++]=0xb8+reg;put32(p+n,rnd());n+=4;}
            else if(op==1){p[n++]=alur[rnd()%(sizeof(alur)/sizeof(alur[0]))];p[n++]=0xc0|(rnd()&63);}
            else if(op==2||op==3){p[n++]=op==2?0x81:0x83;p[n++]=0xc0|(groups[rnd()%6]<<3)|reg;
                if(op==2){put32(p+n,rnd());n+=4;}else p[n++]=rnd();}
            else if(op==4){p[n++]=accum[rnd()%7];put32(p+n,rnd());n+=4;}
            else p[n++]=0x90;
        }
        check(p,n,regs,rnd());
    }
    puts("PASS random register/flags programs: 2000 x 3 interpreter/JIT comparisons");
    /* Partial flag producers, all widths, carry consumers, shifts and rotates
     * are deliberately interpreted; native consumers/producers may surround them. */
    {static const uint8_t fallbacks[][4]={{0x40},{0x48},{0x11,0xc8},{0x19,0xc8},
        {0x66,0x01,0xc8},{0x00,0xe0},{0xd1,0xe0},{0xd1,0xe8},{0xd1,0xf8},
        {0xd1,0xc0},{0xd1,0xc8},{0xd1,0xd0},{0xd1,0xd8},{0x9f},{0x9e}};
     static const unsigned sizes[]={1,1,2,2,3,2,2,2,2,2,2,2,2,1,1};
     for(unsigned f=0;f<sizeof(sizes)/sizeof(sizes[0]);f++)for(unsigned value=0;value<64;value++){
        unsigned n=0;for(unsigned k=0;k<8;k++)regs[k]=rnd();
        p[n++]=0x90;p[n++]=0x90;p[n++]=0x05;put32(p+n,rnd());n+=4;
        memcpy(p+n,fallbacks[f],sizes[f]);n+=sizes[f];
        /* Jcc skips one MOV without loops. All 16 conditions. */
        p[n++]=0x90;p[n++]=0x90;p[n++]=0x89;p[n++]=0xda;
        p[n++]=0x70+(value&15);p[n++]=5;p[n++]=0xbb;put32(p+n,0x12345678);n+=4;
        p[n++]=0x89;p[n++]=0xda;
        check(p,n,regs,rnd());
     }}
    puts("PASS fallback flags/widths/ADC/SBB/shifts/rotates/LAHF/SAHF + all 16 Jcc: 960 x 3");
    { static const uint8_t ops[][2]={{0xf7,0xe1},{0xf7,0xe9},{0xf7,0xf1},{0xf7,0xf9}};
      for(unsigned op=0;op<4;op++)for(unsigned value=0;value<32;value++){
        unsigned n=0;for(unsigned k=0;k<8;k++)regs[k]=rnd();
        regs[0]=rnd()&0x7fffffffu;regs[1]=(rnd()&0x7fffffffu)|1;regs[2]=0;
        p[n++]=0x90;p[n++]=0x90;p[n++]=0x90;
        memcpy(p+n,ops[op],2);n+=2;p[n++]=0x90;p[n++]=0x90;p[n++]=0x90;
        check(p,n,regs,0x202);
      }
      puts("PASS MUL/IMUL/DIV/IDIV interpreter fallback surrounded by native blocks: 128 x 3"); }
    { uint8_t memory[]={0xb8,0,0,2,0,0xb9,0x78,0x56,0x34,0x12,0x90,
                       0x89,0x08,0x8b,0x10,0x83,0xc2,1,0x89,0xd3,0x90};
      memset(regs,0,sizeof regs);check(memory,sizeof memory,regs,0x202);
      uint8_t am[4],bm[4];assert(x86_read_mem(&a,DATA,am,4)&&x86_read_mem(&b,DATA,bm,4));
      assert(!memcmp(am,bm,4)&&am[0]==0x78&&am[3]==0x12);
      puts("PASS memory store/load fallback and native successor preserve little-endian data"); }

}
static void safety_tests(void)
{
    uint8_t p[]={0xb8,1,0,0,0,0x05,2,0,0,0,0x89,0xc3};uint32_t r[8]={0};
    x86jit_stats before,after;
    check(p,sizeof p,r,0x202);x86_jit_snapshot(&b,&before);
#if XJ_NATIVE
    {x86jit_stats ticks={0};
     assert(xj_tick_delta(&ticks,0xfffffffeu,2)==4);
     assert(xj_tick_delta(&ticks,0x7fffffffu,0x80000002u)==3);
     assert(xj_tick_delta(&ticks,100,99)==0&&ticks.clock_invalid==1);
     assert(xj_tick_delta(&ticks,100,111)==11&&ticks.clock_invalid==1);
     puts("PASS profiling clock wrap and regressing sample rejection");}
    { extern int qrx_test_callee_saved(void (*fn)(x86cpu *),x86cpu *);
      xj_block *block=b.jit->slots[(CODE^(CODE>>7))&(XJ_SLOTS-1)].block;
      assert(block && block->code && qrx_test_callee_saved((void (*)(x86cpu *))block->code,&b));
      puts("PASS generated entry preserves D2-D7/A2-A6 and the C stack ABI"); }
#endif
    /* Raw pointer bypasses every software write barrier: entry validation must see it. */
    x86_host_ptr(&b,CODE)[1]=9;x86_host_ptr(&a,CODE)[1]=9;
    p[1]=9;check(p,sizeof p,r,0x202);x86_jit_snapshot(&b,&after);
    assert(after.invalidations>before.invalidations&&b.r[0]==11);
    puts("PASS SMC: raw-host-pointer code mutation invalidates compiled block");
    for(unsigned budget=1;budget<=3;budget++){
        uint64_t ai=x86_icount(&a),bi=x86_icount(&b);
        reset(&a,r,0x202);reset(&b,r,0x202);a.budget=ai+budget;b.budget=bi+budget;
        int fa=reference_run(&a,CODE+sizeof p),fb=x86_run(&b,CODE+sizeof p);
        assert(fa==fb&&memcmp(a.r,b.r,32)==0&&a.eip==b.eip);
        assert(x86_icount(&a)-ai==budget&&x86_icount(&b)-bi==budget);
    }
    puts("PASS exact watchdog at every instruction inside a warmed block");
    reset(&a,r,0x202);reset(&b,r,0x202);
    assert(!reference_run(&a,CODE+5)&&!x86_run(&b,CODE+5));assert(!memcmp(a.r,b.r,32));
    puts("PASS stop EIP inside a warmed translation block");
    /* Mapping is replaced while the old native block is still cached. */
    x86_unmap(&a,CODE,4096);x86_unmap(&b,CODE,4096);
    assert(x86_map(&a,CODE,4096,1)&&x86_map(&b,CODE,4096,1));p[1]=25;
    check(p,sizeof p,r,0x202);assert(b.r[0]==27);
    puts("PASS unmap/remap rejects stale cached instructions");
    { uint8_t negative[]={0xb8,1,0,0,0,0x43};
      check(negative,sizeof negative,r,0x202);
      x86_jit_snapshot(&b,&before);
      for(unsigned i=0;i<100;i++)check(p,sizeof p,r,0x202);
      x86_jit_snapshot(&b,&after);assert(after.native_instructions>before.native_instructions);
      puts("PASS negative cache re-probes modified/newly eligible code without global flush"); }
    { const uint8_t nops[]={0x90,0x90,0x90};uint32_t start=0xfffffffdu;
      assert(x86_map(&a,0xfffff000u,4096,1)&&x86_map(&b,0xfffff000u,4096,1));
      assert(x86_write_mem(&a,start,nops,3)&&x86_write_mem(&b,start,nops,3));
      for(unsigned i=0;i<3;i++){
        reset(&a,r,0x202);reset(&b,r,0x202);a.eip=b.eip=start;
        assert(!reference_run(&a,0)&&!x86_run(&b,0));
      }
      reset(&a,r,0x202);reset(&b,r,0x202);a.eip=b.eip=start;
      uint64_t ai=x86_icount(&a),bi=x86_icount(&b);
      assert(!reference_run(&a,0xfffffffeu)&&!x86_run(&b,0xfffffffeu));
      assert(a.eip==b.eip&&x86_icount(&a)-ai==1&&x86_icount(&b)-bi==1);
      puts("PASS stop EIP inside a block spanning the 32-bit address wrap"); }
    /* Truncated instructions at a page boundary must go to the precise interpreter fault. */
    for(unsigned tail=1;tail<5;tail++){
        uint32_t pos=CODE+4096-tail;uint8_t bytes[5]={0xb8,1,2,3,4};
        x86_unmap(&a,CODE+4096,4096);x86_unmap(&b,CODE+4096,4096);
        assert(x86_write_mem(&a,pos,bytes,tail)&&x86_write_mem(&b,pos,bytes,tail));
        for(unsigned k=0;k<3;k++){
            reset(&a,r,0x202);reset(&b,r,0x202);a.eip=b.eip=pos;
            uint64_t ai=x86_icount(&a),bi=x86_icount(&b);
            int fa=reference_run(&a,0xffffffff),fb=x86_run(&b,0xffffffff);
            assert(x86_icount(&a)-ai==x86_icount(&b)-bi);
            assert(fa==X86_FAULT_PAGE&&fb==fa&&a.fault_eip==b.fault_eip&&a.fault_addr==b.fault_addr);
        }
    }
    puts("PASS page-boundary decode faults remain interpreted and precise");
}
/* Avoid Debian's 68020-only printf division sequence on the 68060 test CPU. */
#if QRX_JIT_KEEP_POSITIVE && XJ_NATIVE
static void positive_cache_retention(void)
{
    uint8_t p[]={0xb8,1,0,0,0,0x05,2,0,0,0,0x89,0xc3};
    uint8_t cold[]={0xb8,0,0,0,0,0x0f,0x0b};
    uint32_t regs[8]={0},other=CODE+8192;
    unsigned slot=(CODE^(CODE>>7))&(XJ_SLOTS-1);
    check(p,sizeof p,regs,0x202);
    xj_block *published=b.jit->slots[slot].block;
    assert(published&&published->eip==CODE);
    while(other<CODE+65536 && ((other^(other>>7))&(XJ_SLOTS-1))!=slot)other++;
    assert(other<CODE+65536);
    assert(x86_map(&b,CODE+8192,65536-8192,1)&&x86_write_mem(&b,other,cold,sizeof cold));
    reset(&b,regs,0x202);
    for(unsigned i=0;i<b.jit->threshold+8;i++){
        b.eip=other;
        assert(!xj_try(&b,other+sizeof cold,other,0));
        assert(b.jit->slots[slot].block==published);
    }
    /* Retention cannot hide actual self modification of the protected block. */
    uint64_t invalid=b.jit->stats.invalidations;
    x86_host_ptr(&b,CODE)[1]^=1;
    b.eip=CODE;
    assert(!xj_try(&b,CODE+sizeof p,CODE,0));
    assert(b.jit->stats.invalidations==invalid+1);
    puts("PASS positive code survives cold/failed colliding probes; raw SMC still invalidates it");
}
#endif

#if QRX_JIT_NATIVE_MEMORY
static void native_memory_guards(void)
{
    uint8_t moves[]={0x8b,0x03,0x83,0xc0,3,0x0f,0xb6,0x53,1,
                     0x01,0xc2,0x89,0x53,8,0x0f,0xbe,0x63,2,0x90,0x90};
    uint8_t faultread[]={0xb8,1,0,0,0,0x83,0xc0,2,0x8b,0x13,0x90};
    uint8_t carries[]={0x8b,0x03,0x40,0x11,0xc0,0x89,0x43,12,0x90,0x90};
    uint8_t aliases[]={0x0f,0xb6,0xc4,0x0f,0xbe,0xd5,0x0f,0xbf,0xd2,0x88,0x63,16,
                       0x8a,0x6b,16,0x0f,0xbf,0xd1,0x0f,0xb7,0x4b,32,0x0f,0xbf,0x53,34,0x90};
    uint8_t selfwrite[]={0xb8,0xba,7,0,0,0xb9,4,0,0,0,0x89,0x03,
                        0xb8,1,0,0,0,0x83,0xc0,1,0x90};
    uint32_t regs[8]={0};uint64_t native=b.jit->stats.native_instructions;
    assert(x86_map(&a,DATA,8192,1)&&x86_map(&b,DATA,8192,1));
    for(unsigned i=0;i<8192;i++){
        *x86_host_ptr(&a,DATA+i)=(uint8_t)(i*29+7);
        *x86_host_ptr(&b,DATA+i)=(uint8_t)(i*29+7);
    }
    check_watchdog=10000;
    puts("MEMORY-GUARD initialized");fflush(stdout);
    regs[X86_EBX]=DATA;
    check(moves,sizeof moves,regs,0x202);
    puts("MEMORY-GUARD warm moves passed");fflush(stdout);
    tlb_flush(&b); /* warmed native block, cold TLB: zero-progress bailout */
    check(moves,sizeof moves,regs,0x202);
    assert(!memcmp(x86_host_ptr(&a,DATA),x86_host_ptr(&b,DATA),4096));
    assert(b.jit->stats.native_instructions>native);

    puts("MEMORY-GUARD cold moves passed");fflush(stdout);
    {
        extern int qrx_test_callee_saved(void (*fn)(x86cpu *),x86cpu *);
        xj_block *block=b.jit->slots[(CODE^(CODE>>7))&(XJ_SLOTS-1)].block;
        assert(block&&block->memory&&qrx_test_callee_saved((void (*)(x86cpu *))block->code,&b));
        tlb_flush(&b);
        assert(qrx_test_callee_saved((void (*)(x86cpu *))block->code,&b));
    }
    for(unsigned cf=0;cf<2;cf++){
        regs[X86_EAX]=0x80ff7f01;regs[X86_ECX]=0xf001;regs[X86_EDX]=0x81aa55f0;
        check(carries,sizeof carries,regs,0x202|cf);
        tlb_flush(&b);check(carries,sizeof carries,regs,0x202|cf);
        check(aliases,sizeof aliases,regs,0x202|cf);
        assert(!memcmp(x86_host_ptr(&a,DATA),x86_host_ptr(&b,DATA),4096));
    }
    puts("MEMORY-GUARD byte aliases, carry chains and saved-D2 ABI passed");fflush(stdout);
    /* Warm a block whose later load can fail, then remove only its data page.
     * Earlier native effects, lazy flags and budget accounting must survive. */
    check(faultread,sizeof faultread,regs,0x202);
    x86_unmap(&a,DATA+4096,4096);x86_unmap(&b,DATA+4096,4096);
    regs[X86_EBX]=DATA+4094;
    for(unsigned repeat=0;repeat<3;repeat++){
        uint64_t ai=x86_icount(&a),bi=x86_icount(&b);int fa,fb;
        assert(x86_write_mem(&a,CODE,faultread,sizeof faultread)&&x86_write_mem(&b,CODE,faultread,sizeof faultread));
        reset(&a,regs,0x202);reset(&b,regs,0x202);
        fa=reference_run(&a,CODE+sizeof faultread);fb=x86_run(&b,CODE+sizeof faultread);
        assert(fa==X86_FAULT_PAGE&&fb==fa);
        assert(a.eip==b.eip&&a.fault_eip==b.fault_eip&&a.fault_addr==b.fault_addr);
        assert(!memcmp(a.r,b.r,32)&&x86_get_flags(&a)==x86_get_flags(&b));
        assert(x86_icount(&a)-ai==x86_icount(&b)-bi);
    }
    puts("MEMORY-GUARD cross-page passed");fflush(stdout);
    /* Writable-TLB tags are revoked by protect. A rejected store must fault
     * through the interpreter, after exactly the preceding native effects. */
    regs[X86_EBX]=DATA;check(moves,sizeof moves,regs,0x202);
    assert(x86_protect(&a,DATA,4096,0)&&x86_protect(&b,DATA,4096,0));
    {
        uint64_t ai=x86_icount(&a),bi=x86_icount(&b);int fa,fb;
        assert(x86_write_mem(&a,CODE,moves,sizeof moves)&&x86_write_mem(&b,CODE,moves,sizeof moves));
        reset(&a,regs,0x202);reset(&b,regs,0x202);
        fa=reference_run(&a,CODE+sizeof moves);fb=x86_run(&b,CODE+sizeof moves);
        assert(fa==X86_FAULT_PAGE&&fb==fa&&a.eip==b.eip&&a.fault_addr==b.fault_addr);
        assert(!memcmp(a.r,b.r,32)&&x86_get_flags(&a)==x86_get_flags(&b));
        assert(x86_icount(&a)-ai==x86_icount(&b)-bi);
    }
    assert(x86_protect(&a,DATA,4096,1)&&x86_protect(&b,DATA,4096,1));

    puts("MEMORY-GUARD readonly passed");fflush(stdout);
    /* Compile with an ordinary data destination, then point the same cached
     * store at its next opcode. The native store itself runs on a warm TLB. */
    regs[X86_EBX]=DATA+128;check(selfwrite,sizeof selfwrite,regs,0x202);
    assert(x86_write_mem(&a,CODE,selfwrite,sizeof selfwrite)&&x86_write_mem(&b,CODE,selfwrite,sizeof selfwrite));
    regs[X86_EBX]=CODE+12;
    (void)wptr(&b,CODE+12);
    reset(&a,regs,0x202);reset(&b,regs,0x202);
    {
        uint64_t ai=x86_icount(&a),bi=x86_icount(&b);
        assert(!reference_run(&a,CODE+sizeof selfwrite)&&!x86_run(&b,CODE+sizeof selfwrite));
        assert(!memcmp(a.r,b.r,32)&&a.r[X86_EDX]==7&&a.r[X86_EAX]==0x7bb);
        assert(a.eip==b.eip&&x86_get_flags(&a)==x86_get_flags(&b));
        assert(x86_icount(&a)-ai==x86_icount(&b)-bi);
        assert(!memcmp(x86_host_ptr(&a,CODE),x86_host_ptr(&b,CODE),sizeof selfwrite));
    }
    /* The same host code page can be writable through another guest address.
     * A guest-address-only SMC test would miss this alias. */
#if QRX_JIT_WRITE_CONTINUE
    regs[X86_EBX]=DATA+128;check(selfwrite,sizeof selfwrite,regs,0x202);
    assert(x86_write_mem(&a,CODE,selfwrite,sizeof selfwrite)&&x86_write_mem(&b,CODE,selfwrite,sizeof selfwrite));
    assert(x86_map_ext(&a,DATA,4096,x86_host_ptr(&a,CODE),1));
    assert(x86_map_ext(&b,DATA,4096,x86_host_ptr(&b,CODE),1));
    (void)wptr(&b,DATA+12);
    regs[X86_EBX]=DATA+12;
    reset(&a,regs,0x202);reset(&b,regs,0x202);
    {
        uint64_t ai=x86_icount(&a),bi=x86_icount(&b);
        assert(!reference_run(&a,CODE+sizeof selfwrite)&&!x86_run(&b,CODE+sizeof selfwrite));
        assert(!memcmp(a.r,b.r,32)&&b.r[X86_EAX]==0x7bb&&b.r[X86_EDX]==7);
        assert(x86_icount(&a)-ai==x86_icount(&b)-bi);
        assert(!memcmp(x86_host_ptr(&a,CODE),x86_host_ptr(&b,CODE),sizeof selfwrite));
    }
    x86_unmap(&a,DATA,4096);x86_unmap(&b,DATA,4096);
    assert(x86_map(&a,DATA,4096,1)&&x86_map(&b,DATA,4096,1));
    puts("MEMORY-GUARD virtual alias cannot hide self modification");fflush(stdout);
#endif
    check_watchdog=0;
    puts("PASS emitted native memory: cold/mid-block TLB deopt, cross-page fault, readonly store, raw SMC successor, exact state/budget");
}
#endif

#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_MEMORY_CMP && XJ_NATIVE
static void native_memory_comparisons(void)
{
    static const uint32_t values[]={0,1,0xffffffffu,0x80000000u,0x7fffffffu,0x55aa55aau};
    uint8_t p[64];uint32_t regs[8]={0};uint64_t native=b.jit->stats.native_instructions;
    assert(x86_map(&a,DATA,8192,1)&&x86_map(&b,DATA,8192,1));
    regs[X86_EBX]=DATA;
    for(unsigned form=0;form<4;form++)for(unsigned v=0;v<6;v++)for(unsigned cc=0;cc<16;cc++){
        unsigned n=0;
        uint32_t right=values[(v+1)%6];
        assert(x86_wr32(&a,DATA,values[v])&&x86_wr32(&b,DATA,values[v]));
        regs[X86_EDX]=right;
        p[n++]=0x90;p[n++]=0x90;
        if(form<2){p[n++]=form?0x3b:0x39;p[n++]=0x13;}
        else {p[n++]=form==2?0x81:0x83;p[n++]=0x3b;
            if(form==2){put32(p+n,right);n+=4;}else p[n++]=(uint8_t)right;}
        p[n++]=0x70+cc;p[n++]=5;
        p[n++]=0xb8;put32(p+n,0x12345678);n+=4;
        p[n++]=0x89;p[n++]=0xc7;
        check(p,n,regs,0x202);
        if(!(cc&3)){tlb_flush(&b);check(p,n,regs,0x202);}
    }
    /* The compare's branch result must survive subsequent guarded loads,
     * stores, and their reuse of A1/CCR. Exercise every condition, including
     * parity's deliberate C fallback, and both branch directions. */
    for(unsigned v=0;v<6;v++)for(unsigned cc=0;cc<16;cc++){
        unsigned n=0;regs[X86_EBX]=DATA;regs[X86_EDX]=values[(v+1)%6];
        assert(x86_wr32(&a,DATA,values[v])&&x86_wr32(&b,DATA,values[v]));
        p[n++]=0x90;p[n++]=0x90;p[n++]=0x3b;p[n++]=0x13;
        p[n++]=0x0f;p[n++]=0xb6;p[n++]=0x03; /* movzx eax,byte [ebx] */
        p[n++]=0x88;p[n++]=0x43;p[n++]=8; /* mov [ebx+8],al */
        p[n++]=0x70+cc;p[n++]=5;
        p[n++]=0xb8;put32(p+n,0x12345678);n+=4;
        p[n++]=0x89;p[n++]=0xc7;
        check(p,n,regs,0x202);tlb_flush(&b);check(p,n,regs,0x202);
    }
    assert(b.jit->stats.native_instructions>native);
    /* A failed CMP read must still expose the preceding ADD flags, even
     * though a successful comparison would overwrite them. */
    {uint8_t q[]={0xb8,1,0,0,0,0x83,0xc0,2,0x3b,0x13,0x90};
     regs[X86_EBX]=DATA;check(q,sizeof q,regs,0x202);
     x86_unmap(&a,DATA+4096,4096);x86_unmap(&b,DATA+4096,4096);
     regs[X86_EBX]=DATA+4094;
     assert(x86_write_mem(&a,CODE,q,sizeof q)&&x86_write_mem(&b,CODE,q,sizeof q));
     reset(&a,regs,0x202);reset(&b,regs,0x202);
     uint64_t ai=x86_icount(&a),bi=x86_icount(&b);
     assert(reference_run(&a,CODE+sizeof q)==X86_FAULT_PAGE);
     assert(x86_run(&b,CODE+sizeof q)==X86_FAULT_PAGE);
     assert(!memcmp(a.r,b.r,32)&&a.eip==b.eip&&a.fault_addr==b.fault_addr&&a.fault_eip==b.fault_eip);
     assert(x86_get_flags(&a)==x86_get_flags(&b)&&x86_icount(&a)-ai==x86_icount(&b)-bi);
    }
    puts("PASS native memory CMP: both operand orders, imm8/imm32, all Jcc, overflow edges, precise fault flags");
}
#endif

#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_LOOP_BATCH && XJ_NATIVE
static void native_loop_batches(void)
{
    /* Six-instruction self loop: guarded load/store, carry-preserving INC,
     * conditional backedge. Compare every budget through the 256-iteration
     * batch boundary against the unchanged reference interpreter. */
    uint8_t p[]={0x8b,0x04,0x93,0x01,0xc8,0x89,0x04,0x97,0x42,0x39,0xea,0x75,0xf3};
    uint32_t regs[8]={0};regs[X86_EBX]=DATA;regs[X86_EDI]=DATA+4096;
    regs[X86_ECX]=3;regs[X86_EBP]=300;
    assert(x86_map(&a,DATA,8192,1)&&x86_map(&b,DATA,8192,1));
    check(p,sizeof p,regs,0x202);
    xj_block *block=b.jit->slots[(CODE^(CODE>>7))&(XJ_SLOTS-1)].block;
    assert(block&&block->eip==CODE&&block->loop_batch&&block->count==6);
    for(unsigned budget=1;budget<=6*260;budget++){
        reset(&a,regs,0x202);reset(&b,regs,0x202);
        uint64_t ai=x86_icount(&a),bi=x86_icount(&b);
        a.budget=ai+budget;b.budget=bi+budget;
        int fa=reference_run(&a,CODE+sizeof p),fb=x86_run(&b,CODE+sizeof p);
        assert(fa==fb&&memcmp(a.r,b.r,32)==0&&a.eip==b.eip&&a.cur_eip==b.cur_eip);
        assert(x86_get_flags(&a)==x86_get_flags(&b));
        assert(x86_icount(&a)-ai==budget&&x86_icount(&b)-bi==budget);
    }
    /* A TLB miss after completed native iterations must retain exact progress
     * and flags. The fourth input crosses into an unmapped page. */
    x86_unmap(&a,DATA+8192,4096);x86_unmap(&b,DATA+8192,4096);
    regs[X86_EBX]=DATA+8192-16;regs[X86_EDI]=DATA;
    reset(&a,regs,0x202);reset(&b,regs,0x202);
    uint64_t ai=x86_icount(&a),bi=x86_icount(&b);
    int fa=reference_run(&a,CODE+sizeof p),fb=x86_run(&b,CODE+sizeof p);
    assert(fa==X86_FAULT_PAGE&&fa==fb&&!memcmp(a.r,b.r,32)&&a.eip==b.eip);
    assert(a.fault_eip==b.fault_eip&&a.fault_addr==b.fault_addr);
    assert(x86_get_flags(&a)==x86_get_flags(&b)&&x86_icount(&a)-ai==x86_icount(&b)-bi);
    puts("PASS native loop batching: exact budgets 1..1560, batch cap, guarded memory, partial-loop fault state");
}
#endif

#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_PAGE_CACHE && XJ_NATIVE
static void native_page_remapping(void)
{
    static uint8_t aa[4096],bb[4096];
    uint8_t p[]={0x8b,0x03,0x83,0xc0,3,0x89,0x43,8,0x89,0xc2};
    uint32_t regs[8]={0};regs[X86_EBX]=DATA;
    assert(x86_map(&a,DATA,4096,1)&&x86_map(&b,DATA,4096,1));
    check(p,sizeof p,regs,0x202);
    assert(x86_map_ext(&a,DATA,4096,aa,1)&&x86_map_ext(&b,DATA,4096,bb,1));
    for(unsigned i=0;i<4096;i++)aa[i]=bb[i]=(uint8_t)(i*17+9);
    check(p,sizeof p,regs,0x202);
    assert(!memcmp(aa,bb,4096)&&a.r[0]==ld32(aa)+3);
    aa[0]^=77;bb[0]^=77;check(p,sizeof p,regs,0x202);
    assert(!memcmp(aa,bb,4096));
    x86_unmap(&a,DATA,4096);x86_unmap(&b,DATA,4096);
    assert(x86_map(&a,DATA,4096,1)&&x86_map(&b,DATA,4096,1));
    assert(x86_wr32(&a,DATA,0x12345678)&&x86_wr32(&b,DATA,0x12345678));
    check(p,sizeof p,regs,0x202);assert(a.r[0]==0x1234567b&&b.r[0]==a.r[0]);
    puts("PASS native page cache: warmed host remap, external pages, raw data edits, unmap/reallocation");
}
#endif

static void metric(const char *name, uint64_t value)
{
    char digits[24];unsigned n=0;
    do{digits[n++]=(char)('0'+value%10);value/=10;}while(value);
    fputs(name,stdout);fputc('=',stdout);while(n)fputc(digits[--n],stdout);fputc(' ',stdout);
}
static void benchmark(unsigned loops)
{
    uint8_t p[]={0xb9,0,0,0,0,0xb8,1,0,0,0,
        0x05,3,0,0,0,0x35,0x55,0x55,0,0,0x89,0xc2,0x01,0xd3,0x83,0xe9,1,0x75,0xed};
    uint32_t r[8]={0};clock_t t;clock_t ticks[2],warm_ticks;uint64_t counts[2];
    unsigned profile=b.jit->stats.profile;
    x86_jit_disable(&b);assert(x86_jit_enable(&b,profile,0));
    b.jit->stats.cache_profile=1;b.jit->threshold=16; /* fresh cache, production hotness threshold */
    put32(p+1,loops);
    for(unsigned mode=0;mode<2;mode++){
        x86cpu *c=mode?&b:&a;assert(x86_write_mem(c,CODE,p,sizeof p));reset(c,r,0x202);
        uint64_t start=x86_icount(c);t=clock();assert(!x86_run(c,CODE+sizeof p));
        ticks[mode]=clock()-t;counts[mode]=x86_icount(c)-start;
    }
    assert(counts[0]==counts[1]&&!memcmp(a.r,b.r,32)&&x86_get_flags(&a)==x86_get_flags(&b));
    /* Warm the hot loop; never-hot entry instructions remain interpreted. */
    reset(&b,r,0x202);assert(!x86_run(&b,CODE+sizeof p));
    x86jit_stats cold,warm;x86_jit_snapshot(&b,&cold);
    reset(&b,r,0x202);t=clock();assert(!x86_run(&b,CODE+sizeof p));warm_ticks=clock()-t;
    x86_jit_snapshot(&b,&warm);assert(warm.compilations==cold.compilations);
    assert(!memcmp(a.r,b.r,32)&&x86_get_flags(&a)==x86_get_flags(&b));
    fputs("BENCH ",stdout);metric("interpreter_ticks",ticks[0]);metric("jit_ticks",ticks[1]);
    metric("warm_ticks",warm_ticks);metric("ticks_per_second",CLOCKS_PER_SEC);metric("guest_instructions",counts[0]);fputc('\n',stdout);
}
static int jit_test_main(int argc,char **argv)
{
#if XJ_NATIVE
    setbuf(stdout,NULL);
    unsigned profile=argc>1?strtoul(argv[1],0,0):20;
    if(argc>1&&!strcmp(argv[1],"--emit")){
        uint8_t buf[XJ_CODE_CAP];xj_emit e={buf,0,0};
        xj_w16(&e,0x206f);xj_w16(&e,4);xj_load(&e,0,2);xj_load(&e,1,3);
        xj_w16(&e,0xd081);xj_w16(&e,0x9081);xj_w16(&e,0xc081);xj_w16(&e,0x8081);xj_w16(&e,0xb380);
        xj_store(&e,0,16);xj_store_imm(&e,0x12345678,20);xj_w16(&e,0x4e75);
        return fwrite(buf,1,e.n,stdout)==e.n?0:1;
    }
    puts("JIT-TEST initializing");init(profile);puts("JIT-TEST random register programs");random_tests();extended_register_tests();carry_tests();condition_and_step_tests();memory_decode_tests();leading_load_tests();safety_tests();
#if QRX_JIT_KEEP_POSITIVE
    positive_cache_retention();
#endif
#if QRX_JIT_NATIVE_MEMORY
    native_memory_guards();
#endif
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_MEMORY_CMP
    native_memory_comparisons();
#endif
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_LOOP_BATCH
    native_loop_batches();
#endif
#if QRX_JIT_NATIVE_MEMORY && QRX_JIT_PAGE_CACHE
    native_page_remapping();
#endif
    /* Restore the second code page after boundary tests. */
    assert(x86_map(&a,CODE+4096,4096,1)&&x86_map(&b,CODE+4096,4096,1));
    benchmark(200000);
    x86jit_stats s;x86_jit_snapshot(&b,&s);assert(s.native_instructions>1000000&&s.compilations&&s.hits);
    fputs("STATS ",stdout);metric("profile",s.profile);metric("native_insns",s.native_instructions);
    metric("fallback",s.fallback_instructions);metric("compilations",s.compilations);
    metric("hits",s.hits);metric("misses",s.misses);metric("invalidations",s.invalidations);
    metric("evictions",s.evictions);metric("code_bytes",s.code_bytes);metric("compile_ticks",s.compile_ticks);metric("peak_code_bytes",s.peak_code_bytes);fputc('\n',stdout);
    x86_free(&a);x86_free(&b);puts("PASS native m68k JIT differential/safety suite");return 0;
#else
    (void)argc;(void)argv;
    x86_init(&a);assert(!x86_jit_enable(&a,20,0)&&!a.jit);x86_free(&a);
    puts("PASS unsupported host fails closed (no native m68k execution)");return 0;
#endif
}

#include "../core/qrx_bigstack.h"
QRX_BIGSTACK_MAIN(jit_test_main)
