#ifndef __APPLE__
#define _POSIX_C_SOURCE 200809L
#else
#define _DARWIN_C_SOURCE
#endif
#include "qrx68_hle.h"
#include "qrx_memory.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

static void out(void *unused,uint8_t ch){(void)unused;fputc(ch,stdout);}
/* QRX68/2 live bridge: private per-process event directory, not a command
 * interpreter. Only bounded IDCMP events enter the isolated guest address space.
 * Frame files are atomically replaced. Host service pins them by request ID. */
static int event_order(const void *a,const void *b){return strcmp((const char *)a,(const char *)b);}
static bool apply_events(QRX68 *r,const char *event_dir){
    DIR *dir=opendir(event_dir);if(!dir)return false;
    int dfd=dirfd(dir);struct dirent *de;
    char names[64][16];unsigned count=0;
    while((de=readdir(dir))!=NULL && count<64){
        const char *n=de->d_name;
        if(strlen(n)!=14 || strcmp(n+10,".evt")!=0)continue;
        bool digits=true;for(int i=0;i<10;i++)if(n[i]<'0'||n[i]>'9')digits=false;
        if(!digits)continue;
        memcpy(names[count++],n,15);
    }
    qsort(names,count,sizeof(names[0]),event_order);
    for(unsigned index=0;index<count;index++){
        const char *n=names[index];
        int fd=openat(dfd,n,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if(fd<0)continue;
        struct stat st;char data[160];ssize_t got=-1;
        if(!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size>0 && st.st_size<(off_t)sizeof(data))
            got=read(fd,data,sizeof(data)-1);
        close(fd);
        if(got>0){
            data[got]=0;
            unsigned wi,cls,code,qual;int x,y;char tail;
            if(sscanf(data,"QRX68/2 %u %u %u %u %d %d %c",&wi,&cls,&code,&qual,&x,&y,&tail)==6 &&
               wi<QRX68_MAX_WINDOWS && code<=65535 && qual<=65535 &&
               x>=-32768 && x<=32767 && y>=-32768 && y<=32767){
                (void)qrx68_post_event(r,wi,cls,(uint16_t)code,(uint16_t)qual,(int16_t)x,(int16_t)y);
            }
        }
        unlinkat(dfd,n,0);
    }
    closedir(dir);return true;
}
static bool atomic_frame(const QRX68 *r,const char *path){
    char tmp[4096];if(strlen(path)>4000)return false;
    snprintf(tmp,sizeof(tmp),"%s.%ld.tmp",path,(long)getpid());
    if(!qrx68_save_ppm(r,tmp)){unlink(tmp);return false;}
    if(rename(tmp,path)!=0){unlink(tmp);return false;}
    return true;
}
static int run_live(QRX68 *r,uint64_t budget,const char *events,const char *frame){
    uint64_t last=UINT64_MAX;
    int rc=1;
#ifdef QRX68_MUSASHI_ENABLED
    if(!qrx68_musashi_begin(r))return -1;
#endif
    while(r->steps<budget && !r->failed && !r->complete){
        if(!apply_events(r,events)){rc=-1;break;}
#ifdef QRX68_MUSASHI_ENABLED
        rc=qrx68_musashi_slice(r,2000);
#else
        rc=qrx68_run(r,2000);
#endif
        if(r->framebuffer && r->frame_generation!=last){
            if(!atomic_frame(r,frame)){rc=-1;break;}
            last=r->frame_generation;
        }
        if(rc<=0)break;
        /* Sleeping after an explicit WaitPort prevents busy-spin on real
         * host input without modifying guest clock or emulating a timer. */
        if(rc==2){struct timespec ts={0,20000000L};nanosleep(&ts,NULL);}
    }
#ifdef QRX68_MUSASHI_ENABLED
    qrx68_musashi_end(r);
#endif
    if(r->complete)return 0;
    return (r->failed || rc<0)?-1:1;
}
static void usage(void){
    fprintf(stderr,"usage: qrx68-run [--ram auto|MIB] [--share DIR] [--share-rw] [--budget N] [--experimental-gui --snapshot FILE.ppm --gpu-socket PATH --live-events DIR --live-frame PPM] PROGRAM.HUNK\n"
        "Runs a strictly bounded isolated Amiga 68k CLI HUNK in QRX68 HLE.\n"
        "Unimplemented LVOs and invalid memory always FAIL, never fake success.\n"
        "Build with Musashi to run the full 68040 ISA/FPU; portable CPU is a subset.\n"
        "Host files: single directory, read-only unless --share-rw.\n");
}
int main(int argc,char **argv){
    const char *share=NULL,*file=NULL,*snapshot=NULL,*gpu_socket=NULL;
    const char *live_events=NULL,*live_frame=NULL;
    bool experimental_gui=false;
    bool rw=false;
    uint64_t budget=1000000;
    size_t memory_bytes=qrx_ram_auto_bytes();
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--share") && i+1<argc)share=argv[++i];
        else if(!strcmp(argv[i],"--share-rw"))rw=true;
        else if(!strcmp(argv[i],"--experimental-gui"))experimental_gui=true;
        else if(!strcmp(argv[i],"--snapshot") && i+1<argc)snapshot=argv[++i];
        else if(!strcmp(argv[i],"--gpu-socket") && i+1<argc)gpu_socket=argv[++i];
        else if(!strcmp(argv[i],"--live-events") && i+1<argc)live_events=argv[++i];
        else if(!strcmp(argv[i],"--live-frame") && i+1<argc)live_frame=argv[++i];
        else if(!strcmp(argv[i],"--ram") && i+1<argc){
            const char *value=argv[++i];
            if(!strcmp(value,"auto"))memory_bytes=qrx_ram_auto_bytes();
            else{
                char *end=NULL;errno=0;
                unsigned long long mib=strtoull(value,&end,10);
                if(errno||!end||*end||mib<1||mib>3840){usage();return 2;}
                memory_bytes=(size_t)mib*1048576u;
            }
        }
        else if(!strcmp(argv[i],"--budget")&&i+1<argc){
            char *end=NULL;errno=0;budget=strtoull(argv[++i],&end,10);
            if(errno||!end||*end||!budget||budget>100000000ull){usage();return 2;}
        }
        else if(!strcmp(argv[i],"--help")){usage();return 0;}
        else if(argv[i][0]=='-' || file){usage();return 2;}
        else file=argv[i];
    }
    if(!file || (rw&&!share) || ((snapshot||gpu_socket||live_events||live_frame)&&!experimental_gui) ||
       (!!live_events != !!live_frame)){usage();return 2;}
    struct stat st;
    if(stat(file,&st)||st.st_size<20||st.st_size>64LL*1024*1024){perror("invalid HUNK input");return 2;}
    FILE *f=fopen(file,"rb");if(!f){perror("open");return 2;}
    uint8_t *input=malloc((size_t)st.st_size);
    if(!input || fread(input,1,(size_t)st.st_size,f)!=(size_t)st.st_size){
        fprintf(stderr,"qrx68: failed to read executable\n");free(input);fclose(f);return 2;
    }
    fclose(f);
    QRX68 r;
    if(!qrx68_init(&r,memory_bytes,out,NULL)){
        fprintf(stderr,"qrx68: allocation failed\n");free(input);return 2;
    }
    if(experimental_gui&&!qrx68_enable_gui(&r)){
        fprintf(stderr,"qrx68: cannot allocate experimental framebuffer\n");
        free(input);qrx68_destroy(&r);return 2;
    }
    if(share && !qrx68_share(&r,share,rw)){
        fprintf(stderr,"qrx68: cannot mount shared directory\n");free(input);qrx68_destroy(&r);return 2;
    }
    if(!qrx68_load_hunk(&r,input,(size_t)st.st_size)){
        fprintf(stderr,"qrx68: unsupported or malformed AmigaDOS HUNK\n");
        free(input);qrx68_destroy(&r);return 2;
    }
    free(input);
#ifdef QRX68_MUSASHI_ENABLED
    int rc=live_events ? run_live(&r,budget,live_events,live_frame) : qrx68_run_musashi(&r,budget);
#else
    int rc=live_events ? run_live(&r,budget,live_events,live_frame) : qrx68_run(&r,budget);
#endif
    if(gpu_socket && !qrx68_send_gpu_frame(&r,gpu_socket)){
        fprintf(stderr,"qrx68: QRX GPU socket transmission failed\n");rc=-1;
    }
    if(snapshot && !qrx68_save_ppm(&r,snapshot)){
        fprintf(stderr,"qrx68: unable to save requested snapshot\n");rc=-1;
    }
    fflush(stdout);
#ifdef QRX68_MUSASHI_ENABLED
    fprintf(stderr,"QRX68 CPU=Musashi-68040, ");
#else
    fprintf(stderr,"QRX68 CPU=portable-subset, ");
#endif
    fprintf(stderr,"QRX68: %s, %" PRIu64 " guest steps, %" PRIu64
        " HLE calls, %u unsupported APIs",rc==0?"PASS":rc==1?"BUDGET":"FAIL",
        r.steps,r.hle_calls,r.unsupported);
    if(r.unsupported)fprintf(stderr,", unsupported lib=%u LVO=-%u",r.failed_lib,r.failed_lvo);
    fputc('\n',stderr);
    qrx68_destroy(&r);
    return rc==0?0:1;
}
