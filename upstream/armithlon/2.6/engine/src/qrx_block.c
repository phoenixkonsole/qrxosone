#define _POSIX_C_SOURCE 200809L
#include "qrx_block.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>

bool qrx_block_mount(QRXBlock *b,const char *path,uint32_t sectors,bool create,bool readonly){
    if(!b||!path||!*path||sectors==0||sectors>QRX_BLOCK_MAX_SECTORS)return false;
    memset(b,0,sizeof(*b));
    FILE *f=NULL;
    if(create){
        /* Exclusive create: never silently overwrite an existing disk image. */
        int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);
        if(fd<0)return false;
        if(ftruncate(fd,(off_t)sectors*QRX_BLOCK_SECTOR_SIZE)!=0){
            close(fd);unlink(path);return false;
        }
        f=fdopen(fd,"r+b");
        if(!f){close(fd);unlink(path);return false;}
    }else f=fopen(path,readonly?"rb":"r+b");
    if(!f)return false;
    struct stat st;
    if(fstat(fileno(f),&st)!=0||!S_ISREG(st.st_mode)||
       st.st_size!=(off_t)sectors*QRX_BLOCK_SECTOR_SIZE){fclose(f);return false;}
    b->file=f;b->sectors=sectors;b->readonly=readonly;
    return true;
}
void qrx_block_unmount(QRXBlock *b){if(b){if(b->file)fclose(b->file);memset(b,0,sizeof(*b));}}
void qrx_block_attach(QRXVM *vm,QRXBlock *b){if(vm)vm->block=b;}
static bool valid(QRXVM *vm,uint32_t a,unsigned w){
    return vm&&vm->block&&vm->block->file&&w==4&&!(a&3u)&&
           a>=QRX_BLOCK_BASE && a<QRX_BLOCK_BASE+0x1cu;
}
bool qrx_block_read(QRXVM *vm,uint32_t a,unsigned w,uint32_t *out){
    if(!valid(vm,a,w)||!out)return false;
    QRXBlock *b=vm->block;
    switch(a){
    case QRX_BLOCK_MAGIC:*out=0x51525842u;return true;
    case QRX_BLOCK_SECTORS:*out=b->sectors;return true;
    case QRX_BLOCK_LBA:*out=b->lba;return true;
    case QRX_BLOCK_DMA:*out=b->dma;return true;
    case QRX_BLOCK_STATUS:*out=b->status;return true;
    case QRX_BLOCK_READONLY:*out=b->readonly;return true;
    default:return false;
    }
}
bool qrx_block_write(QRXVM *vm,uint32_t a,unsigned w,uint32_t v){
    if(!valid(vm,a,w))return false;
    QRXBlock *b=vm->block;
    if(a==QRX_BLOCK_LBA){b->lba=v;return true;}
    if(a==QRX_BLOCK_DMA){b->dma=v;return true;}
    if(a!=QRX_BLOCK_COMMAND)return false;
    if(v!=1&&v!=2){b->status=1;return true;}
    if(v==2&&b->readonly){b->status=3;return true;}
    if(b->lba>=b->sectors||(uint64_t)b->dma+QRX_BLOCK_SECTOR_SIZE>vm->ram.size){b->status=1;return true;}
    if(fseeko(b->file,(off_t)b->lba*QRX_BLOCK_SECTOR_SIZE,SEEK_SET)!=0){b->status=2;return true;}
    uint8_t *ram=vm->ram.data+b->dma;
    if(v==1){
        if(fread(ram,1,QRX_BLOCK_SECTOR_SIZE,b->file)!=QRX_BLOCK_SECTOR_SIZE){clearerr(b->file);b->status=2;return true;}
        vm->ram_write_epoch++; /* DMA invalidates CPU/JIT translations */
        if(vm->dma_ram_write)vm->dma_ram_write(vm->dma_opaque,b->dma,QRX_BLOCK_SECTOR_SIZE);
    }else{
        if(fwrite(ram,1,QRX_BLOCK_SECTOR_SIZE,b->file)!=QRX_BLOCK_SECTOR_SIZE||
           fflush(b->file)!=0||fsync(fileno(b->file))!=0){b->status=2;return true;}
    }
    b->status=0;return true;
}
