/* CLI for complete Musashi instruction decoding on a QRX virtual board.
 * This is a CPU+bus diagnostic, not a complete Amiga/AROS machine emulator. */
#include "qrx68100_full.h"
#include "m68k.h"
#include "qrx_hostfs.h"
#include "qrx68100_jit.h"
#include "qrx86_door.h"
#include "qrxmedia.h"
#include "qrx68100_core1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#if defined(__APPLE__) || defined(QRX_SDL_FRONTEND)   /* SDL: linux/qrx_aros_window_sdl.c */
#include "../macos/qrx_aros_window.h"
#define QRX_NATIVE_WINDOW 1
#endif
static int native_window;
static uint32_t host_ms(void *ctx){(void)ctx;struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint32_t)((uint64_t)t.tv_sec*1000+t.tv_nsec/1000000);}
/* Guest serial debug: stdio's line buffer instead of one write syscall per byte. */
static void putc_host(void *p,uint8_t ch){(void)p;fputc(ch,stdout);}
/* FSAGA display sink: rept the RTG framebuffer as P6 PPM. The guest buffer is
 * read straight out of the linear guest RAM; a PPM is written only when the
 * mode or geometry changes and at most 2x per wall-clock second to keep disk
 * churn proportional to what is actually on screen. */
struct fsaga_sink { const char *path; uint32_t last_w, last_h, last_fmt;
    long long last_ms; uint32_t written; };
static uint32_t fsaga_bytes_pp(uint32_t fmt) {
    switch (fmt & 0x7Fu) {
        case 1u: return 1u; /* CLUT8 */
        case 2u: case 3u: return 2u; /* RGB16 / RGB15 */
        case 4u: return 3u; /* RGB24 */
        case 5u: return 4u; /* RGB32 */
        default: return 0u;
    }
}
static void fsaga_render(const QRXFullCPU *c,uint32_t addr,uint32_t w,uint32_t h,
                         uint32_t fmt,uint8_t *rgb) { /* rgb = 3 bytes/pixel */
    const uint8_t *ram = c->vm.ram.data;
    const uint8_t *fb = ram + addr;
    uint32_t n = w * h; (void)fmt;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cv;
        switch (fmt & 0x7Fu) {
            case 1u: /* CLUT8 -> FSAGA palette */
                cv = c->fsaga_clut[fb[i] & 0xFFu] & 0xFFFFFFu; break;
            case 2u: { /* RGB16 565, big-endian */
                uint32_t px = ((uint32_t)fb[i*2u] << 8) | fb[i*2u+1u];
                cv = (((px >> 11u) & 31u) << 19u) | (((px >> 5u) & 63u) << 10u) |
                     ((px & 31u) << 3u); break;
            }
            case 3u: { /* RGB15 555, big-endian */
                uint32_t px = ((uint32_t)fb[i*2u] << 8) | fb[i*2u+1u];
                cv = (((px >> 10u) & 31u) << 19u) | (((px >> 5u) & 31u) << 10u) |
                     ((px & 31u) << 3u); break;
            }
            case 4u: /* RGB24, R,G,B bytes */
                cv = ((uint32_t)fb[i*3u] << 16u) | ((uint32_t)fb[i*3u+1u] << 8u) |
                     fb[i*3u+2u]; break;
            case 5u: /* RGB32, big-endian AARRGGBB */
                cv = ((uint32_t)fb[i*4u+1u] << 16u) | ((uint32_t)fb[i*4u+2u] << 8u) |
                     fb[i*4u+3u]; break;
            default: cv = 0; break;
        }
        rgb[i*3u+0u] = (uint8_t)(cv >> 16u);
        rgb[i*3u+1u] = (uint8_t)(cv >> 8u);
        rgb[i*3u+2u] = (uint8_t)cv;
    }
}
static void fsaga_host_display(void *arg, const QRXFullCPU *c, uint32_t frame,
                               uint32_t addr, uint32_t w, uint32_t h, uint32_t fmt) {
    struct fsaga_sink *s = (struct fsaga_sink*)arg;
    unsigned bpp = fsaga_bytes_pp(fmt);
    if (!bpp || !w || !h) return;
    if ((uint64_t)addr + (uint64_t)w * h * bpp > c->vm.ram.size) return;
#ifdef QRX_NATIVE_WINDOW
    /* Zero-copy: the GPU reads guest RAM itself. */
    if(native_window){qrx_aros_window_publish(addr,w*bpp+(int16_t)c->fsaga_bplhmod,w,h,fmt&0x7Fu,c->fsaga_clut);return;}
#endif
    if (c->vm.clock_ms) { long long now = c->vm.clock_ms(c->vm.host_ctx);
        if (s->last_ms > 0 && (uint32_t)(now - s->last_ms) < 500) return;
        s->last_ms = now; }
    size_t need = (size_t)w * h * 3u;
    uint8_t *rgb = malloc(need); if (!rgb) return;
    fsaga_render(c, addr, w, h, fmt, rgb);
    char tmp[256]; snprintf(tmp, sizeof tmp, "%s.tmp", s->path);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        fprintf(stderr, "[FSAGA-IM] frame=%u mode=%04x %ux%u fmt=%u -> %s\n",
            frame, c->fsaga_mode, w, h, fmt, tmp);
        fwrite(rgb, 1u, need, f);
        fclose(f); rename(tmp, s->path); s->written++;
    }
    free(rgb);
    if (w != s->last_w || h != s->last_h || fmt != s->last_fmt) {
        fprintf(stderr, "[FSAGA-SWITCH] mode=%04x %ux%u fmt=%u bplptr=%08x\n",
            c->fsaga_mode, w, h, fmt, addr);
        s->last_w = w; s->last_h = h; s->last_fmt = fmt;
    }
}
static uint8_t *load(const char *p,size_t *size){
    FILE *f=fopen(p,"rb");if(!f)return NULL;
    if(fseek(f,0,SEEK_END)){fclose(f);return NULL;}
    long n=ftell(f);if(n<=0 || n>(long)QRX_FULL_ROM_MAX){fclose(f);return NULL;}
    rewind(f);uint8_t *b=malloc((size_t)n);
    if(!b||fread(b,1,(size_t)n,f)!=(size_t)n){free(b);fclose(f);return NULL;}
    fclose(f);*size=(size_t)n;return b;
}
static uint8_t *load_adf(const char *p,size_t *size){
    FILE *f=fopen(p,"rb");if(!f)return NULL;
    if(fseek(f,0,SEEK_END)){fclose(f);return NULL;}
    long n=ftell(f);if(n<=0 || n>17u*1024u*1024u||(n & 511)){fclose(f);return NULL;}
    rewind(f);uint8_t *b=malloc((size_t)n);
    if(!b||fread(b,1,(size_t)n,f)!=(size_t)n){free(b);fclose(f);return NULL;}
    fclose(f);*size=(size_t)n;return b;
}
static atomic_bool stop_requested,cpu_finished;
/* Headless check of the AHI path: drains the media ring to a raw file at the
 * rate CoreAudio would (QRX_AUDIO_DUMP, 48 kHz stereo 16-bit host-endian). */
static QRXMediaDevice dump_media;
static pthread_t dump_tid;
static bool dump_running;
static void *audio_dump_thread(void *arg){
    FILE *f=arg;
    int16_t pcm[480*2];
    while(!atomic_load(&cpu_finished)){
        struct timespec nap={0,10000000};
        nanosleep(&nap,NULL);
        size_t got=qrx_media_pull_audio(&dump_media,pcm,480);
        if(got)fwrite(pcm,4,got,f);
    }
    fclose(f);
    return NULL;
}
static double run_seconds; /* wall-clock limit for headless timing runs */
struct cpu_run { QRXFullCPU *c; uint64_t budget; int rc; };
/* The whole emulation loop; on its own thread with a native window so AppKit
 * tracking loops and display sync never stall the guest. */
static void *cpu_thread(void *arg){
    struct cpu_run *r=arg;
    QRXFullCPU *c=r->c;
    uint64_t budget=r->budget;
    int rc=1;
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
#endif
    struct timespec t0;clock_gettime(CLOCK_MONOTONIC,&t0);
    while(budget>0&&rc==1&&!atomic_load_explicit(&stop_requested,memory_order_relaxed)){
        if(run_seconds>0){
            struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
            if((double)(t.tv_sec-t0.tv_sec)+(double)(t.tv_nsec-t0.tv_nsec)*1e-9>=run_seconds)break;
        }
        uint64_t step=native_window?200000u:50000000u;
        uint64_t chunk=budget>step?step:budget;
        rc=qrx_full_run_cycles(c,chunk);
        budget-=chunk;
        if(rc==1 && !native_window && !c->realtime)
            fprintf(stderr,"[ARMITHLON] ... PC=%08x A7=%08x USP=%08x ISP=%08x SR=%04x instructions=%llu cycles=%llu\n",
                qrx_full_pc(c),qrx_full_reg(c,15),m68k_get_reg(NULL,M68K_REG_USP),m68k_get_reg(NULL,M68K_REG_ISP),
                m68k_get_reg(NULL,M68K_REG_SR)&0xFFFFu,(unsigned long long)c->instructions,(unsigned long long)c->cycles);
    }
    r->rc=rc;
    atomic_store(&cpu_finished,true);
    return NULL;
}
int main(int argc,char **argv){
    unsigned cpu=68040;const char *mainrom=NULL,*extrom=NULL,*share=NULL;
    bool share_rw=false;QRXHostFS hostfs={0},appsfs={0};
    const char *apps=NULL;
    bool realtime=false;
    /* A quarter of the host's RAM up to the board limit, backed lazily.
     * C:QRXMoreMemory hands the part above 32 MiB to exec. */
    uint32_t ram_mb=(uint32_t)(qrx_ram_auto_bytes()/(1024u*1024u));
    uint64_t budget=1000000;
    uint8_t *adf_hold=NULL;
    uint8_t *rom=NULL;   /* before any goto done: freed there */
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--cpu")&&i+1<argc)cpu=(unsigned)strtoul(argv[++i],NULL,10);
        else if(!strcmp(argv[i],"--rom")&&i+1<argc)mainrom=argv[++i];
        else if(!strcmp(argv[i],"--ext-rom")&&i+1<argc)extrom=argv[++i];
        else if(!strcmp(argv[i],"--window")){native_window=1;budget=UINT64_MAX;}
        else if(!strcmp(argv[i],"--share")&&i+1<argc)share=argv[++i];
        else if(!strcmp(argv[i],"--share-rw"))share_rw=true;
        else if(!strcmp(argv[i],"--realtime"))realtime=true;
        else if(!strcmp(argv[i],"--ram")&&i+1<argc)ram_mb=(uint32_t)strtoul(argv[++i],NULL,10);
        else if(!strcmp(argv[i],"--seconds")&&i+1<argc){run_seconds=strtod(argv[++i],NULL);budget=UINT64_MAX;}
        else if(!strcmp(argv[i],"--apps")&&i+1<argc)apps=argv[++i];
        else if(!strcmp(argv[i],"--cycles")&&i+1<argc)budget=strtoull(argv[++i],NULL,10);
        else {fprintf(stderr,"usage: qrx68100_full_run [--cpu 68000|68010|68020|68030|68040] [--rom main.bin] [--ext-rom ext.bin] [--share directory] [--share-rw] [--cycles N]\n");return 2;}
    }
    setvbuf(stdout,NULL,_IOLBF,1u<<16);
    QRXFullCPU c;
    size_t adfsz = 0;
    int rc = 1;
    if(ram_mb<32)ram_mb=32;
    if(!qrx_full_init(&c,(size_t)ram_mb*1024u*1024u,cpu)){fprintf(stderr,"CPU init failed; models 68000..68040, one active VM\n");return 2;}
    if(share){
        if(!qrx_hostfs_mount(&hostfs,share,!share_rw)){fprintf(stderr,"HostFS mount failed: %s\n",share);qrx_full_destroy(&c);return 2;}
        qrx_hostfs_attach(&c.vm,&hostfs);
    }
    /* Headless runs need the APPS: door too; the native window mounts its own. */
    if(apps&&!native_window){
        if(!qrx_hostfs_mount(&appsfs,apps,false)){fprintf(stderr,"APPS mount failed: %s\n",apps);qrx_full_destroy(&c);return 2;}
        qrx_hostfs_attach2(&c.vm,&appsfs);
    }
    qrx_full_hide_high_ram(&c,32u*1024u*1024u);
    qrx_full_set_host(&c,putc_host,host_ms,NULL);
#ifdef QRX_NATIVE_WINDOW
    if(native_window){static struct fsaga_sink live;if(!qrx_aros_window_open(&c.vm,c.ram_total?c.ram_total:c.vm.ram.size)){fprintf(stderr,"native window failed\n");return 2;}qrx_full_set_display(&c,fsaga_host_display,&live);}
#endif
    {
        const char *ppm = getenv("QRX_FULL_FSAGA_TEST");
        if (ppm) {
            /* Direct RTG self-test, no ROM: arm SAGA_VIDEO_MODE/CLUT/BPLPTR via the
             * guest bus exactly as a real AROS SAGA driver would and force one
             * present() into the display sink. Proves the whole FSAGA MMIO
             * window plus the host framebuffer conversion path. */
            static struct fsaga_sink sink;
            sink.path = ppm; sink.last_w = 0; sink.last_h = 0; sink.last_fmt = 0;
            sink.last_ms = -1; sink.written = 0;
            qrx_full_set_display(&c, fsaga_host_display, &sink);
            uint32_t w = 320, h = 240;
            uint32_t fbofs = 0x00E00000u; /* in guest linear RAM, above 0xE00000 ext ROM hole */
            for (int i = 0; i < 256; i++) {
                uint32_t color = ((i * 3) & 255) << 16 | (((i * 7) & 255) << 8) | ((i * 11) & 255);
                if (!qrx_full_bus_write(&c, 0x00DFF400u + (uint32_t)(i << 2), 4, color))
                    { fprintf(stderr, "FSAGA test: bus CLUT write %d failed\n", i); break; }
            }
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                    c.ram_owned[fbofs + y * w + x] = (uint8_t)((((x * 137u) ^ (y * 61u)) ^ 0x55u) & 0xFFu);
            if (!qrx_full_bus_write(&c, 0x00DFF3FCu, 2, 0x0005u))
                fprintf(stderr, "FSAGA test: board id read shell\n");
            uint32_t id = 0;
            if (qrx_full_bus_read(&c, 0x00DFF3FCu, 2, &id) && (id >> 8) == 5)
                fprintf(stderr, "[FSAGA] board id = %04x\n", id);
            else fprintf(stderr, "[FSAGA] board id read FAILED v=%08x\n", id);
            if (!qrx_full_bus_write(&c, 0x00DFF300u, 2, w) ||
                !qrx_full_bus_write(&c, 0x00DFF308u, 2, h))
                fprintf(stderr, "FSAGA test: HPIXEL/VPIXEL write failed\n");
            if (!qrx_full_bus_write(&c, 0x00DFF1ECu, 4, fbofs) ||
                !qrx_full_bus_write(&c, 0x00DFF1F4u, 2, 0x0001u /* SAGA_VIDEO_FORMAT_CLUT8 */))
                fprintf(stderr, "FSAGA test: BPLPTR/MODE write failed\n");
            qrx_full_fsaga_present(&c);
            fprintf(stderr, "[FSAGA] self-test wrote %u frame(s) to %s\n", sink.written, ppm);
            rc = 0; goto done;
        }
        {
        const char * ppm0 = getenv("QRX_FULL_FSAGA_PPM");
        if (ppm0) {
            static struct fsaga_sink sink;
            sink.path = ppm0; sink.last_w = 0; sink.last_h = 0; sink.last_fmt = 0;
            sink.last_ms = -1; sink.written = 0;
            qrx_full_set_display(&c, fsaga_host_display, &sink);
            fprintf(stderr, "[ARMITHLON] FSAGA display sink: %s\n", ppm0);
        }
        }
    }
    {
        const char *adfpath = getenv("QRX_FULL_ADF");
        if (adfpath) {
            uint8_t *adf = load_adf(adfpath, &adfsz);
            if (adf) {
                adf_hold = adf; qrx_full_load_adf(&c, adf, adfsz);
            } else fprintf(stderr, "[ARMITHLON] ADF load failed: %s\n", adfpath);
        }
    }
    size_t sz=0;
    if(mainrom){
        rom=load(mainrom,&sz);
        if(!rom||!qrx_full_load_rom(&c,QRX_FULL_ROM_MAIN_BASE,rom,sz)){fprintf(stderr,"invalid main ROM\n");goto done;}
        free(rom);rom=NULL;
        if(extrom){rom=load(extrom,&sz);if(!rom||!qrx_full_load_rom(&c,QRX_FULL_ROM_EXT_BASE,rom,sz)){fprintf(stderr,"invalid ext ROM\n");goto done;}free(rom);rom=NULL;}
        qrx_full_set_rom_overlay(&c,true);
        fprintf(stderr,"[ARMITHLON] AROS boot: RTG, HostFS and AHI host drivers enabled\n");
    }else if(!qrx_build_boot_monitor(c.ram_owned,c.vm.ram.size)){fprintf(stderr,"monitor generation failed\n");goto done;}
    if(!qrx_full_reset(&c)){fprintf(stderr,"invalid vectors / reset failed\n");goto done;}
    qrx86_door_attach(&c);
    /* QRX_JIT=0 keeps Musashi alone. The early-boot delay lets the bus see the
     * ROM's EClock setup, which translated RAM writes do not pass through. */
    {const char *je=getenv("QRX_JIT");
     if(!je||strcmp(je,"0")){
         bool jit_on=qrx_jit_init(&c,mainrom?20000000u:0u);
         if(jit_on)qrx_core1_attach(&c);
         fprintf(stderr,"[ARMITHLON] JIT %s\n",jit_on?"on":"unavailable");
     }}
    fprintf(stderr,"[ARMITHLON] model=%u PC=%08x cycles_budget=%llu\n",cpu,qrx_full_pc(&c),(unsigned long long)budget);
    /* Slice long boots so a visible console shows the reset vector, the
     * running PC and retired instructions while AROS initializes. */
    /* The guest's input and audio drivers expect the media board in every mode. */
    if(!native_window){
        const char *dump=getenv("QRX_AUDIO_DUMP");
        qrx_media_init(&dump_media,NULL,NULL);qrx_vm_bind_media(&c.vm,&dump_media);
        if(dump){
            FILE *f=fopen(dump,"wb");
            dump_running=f&&pthread_create(&dump_tid,NULL,audio_dump_thread,f)==0;
        }
    }
    struct timespec bench_t0;clock_gettime(CLOCK_MONOTONIC,&bench_t0);
    if(realtime||native_window)qrx_full_set_realtime(&c,qrx_jit_cpu_stopped);
    {
        struct cpu_run run={&c,budget,1};
#ifdef QRX_NATIVE_WINDOW
        if(native_window){
            pthread_t tid;
            if(pthread_create(&tid,NULL,cpu_thread,&run)!=0){fprintf(stderr,"CPU thread failed\n");goto done;}
            while(!atomic_load(&cpu_finished)&&qrx_aros_window_pump()){}
            atomic_store(&stop_requested,true);
            pthread_join(tid,NULL);
        }else
#endif
        cpu_thread(&run);
        rc=run.rc;
    }
    if(dump_running)pthread_join(dump_tid,NULL);
    {struct timespec t1;clock_gettime(CLOCK_MONOTONIC,&t1);
     double dt=(double)(t1.tv_sec-bench_t0.tv_sec)+(double)(t1.tv_nsec-bench_t0.tv_nsec)*1e-9;
     fprintf(stderr,"\n[BENCH] seconds=%.3f instructions=%llu mips=%.1f guest_mcycles_per_s=%.1f\n",dt,
        (unsigned long long)c.instructions,dt>0?(double)c.instructions/dt/1e6:0.0,dt>0?(double)c.cycles/dt/1e6:0.0);}
    fprintf(stderr,"\n[ARMITHLON] result=%d PC=%08x D0=%08x A0=%08x A1=%08x instructions=%llu cycles=%llu fault=%d fault_addr=%08x shutdown=%u\n",
        rc,qrx_full_pc(&c),qrx_full_reg(&c,0),qrx_full_reg(&c,8),qrx_full_reg(&c,9),
        (unsigned long long)c.instructions,(unsigned long long)c.cycles,
        c.mem_fault,c.fault_address,c.vm.cpu.halted);
    if(mainrom)rc=rc<0?1:3; /* ROM probe never claims desktop boot success */
    else rc=rc==0?0:1;done:
    { const char *dump = getenv("QRX_FULL_DUMP_RAM");
      if (dump) {
        /* Multiple ranges: "start:len:file", separated by ','. */
        char tmp[1024]; const char *dp = dump;
        while (*dp) {
          unsigned long a = 0, n = 0; char f[256];
          size_t seg = strcspn(dp, ",");
          if (seg >= sizeof(tmp)) seg = sizeof(tmp) - 1;
          memcpy(tmp, dp, seg); tmp[seg] = 0;
          dp += seg;
          while (*dp == ',') dp++;
          if (sscanf(tmp, "%lx:%lx:%255s", &a, &n, f) == 3 && n > 0 &&
              n <= 1024u*1024u*1024u && (uint64_t)a + n <= c.vm.ram.size) {
            FILE *o = fopen(f, "wb");
            if (o) { fwrite(c.ram_owned + a, 1, (size_t)n, o); fclose(o);
              fprintf(stderr, "[ARMITHLON] RAM %08lx+%lx -> %s\n", a, n, f); }
          }
        }
      } }
#ifdef QRX_NATIVE_WINDOW
    if(native_window)qrx_aros_window_close();
#endif
    qrx86_door_report();
    qrx_core1_detach(&c);
    qrx_jit_destroy(&c);
    free(rom);qrx_hostfs_attach(&c.vm,NULL);qrx_hostfs_unmount(&hostfs);
    if(apps&&!native_window){qrx_hostfs_attach2(&c.vm,NULL);qrx_hostfs_unmount(&appsfs);}
    qrx_full_destroy(&c);free(adf_hold);return rc;}
