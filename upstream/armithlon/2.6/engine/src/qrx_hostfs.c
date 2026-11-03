/* Capability-style host share with directory-fd confinement. This is not a
 * general passthrough mount: the legacy flat ABI and the hierarchical ABI share a confined root. */
#ifndef __APPLE__
#define _POSIX_C_SOURCE 200809L
#else
#define _DARWIN_C_SOURCE
#endif
#include "qrx_hostfs.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifndef O_NOFOLLOW
#error "QRXHostFS needs O_NOFOLLOW to protect against symlink traversal"
#endif
static int bounds(const QRXVM *v,uint32_t p,uint32_t n){
    return v && v->ram.data && n && (uint64_t)p+n<=v->ram.size;
}
static int safe_name(QRXVM *v, QRXHostFS *fs,char out[QRX_HOSTFS_MAX_NAME+1]){
    if(!bounds(v,fs->name_dma,QRX_HOSTFS_MAX_NAME+1))return 0;
    const uint8_t *p=v->ram.data+fs->name_dma;
    size_t n=0;
    for(;n<=QRX_HOSTFS_MAX_NAME&&p[n];++n){
        const unsigned char c=p[n];
        if(!(isalnum(c)||c=='-'||c=='_'||c=='.'||c==' '))return 0;
    }
    if(!n || n>QRX_HOSTFS_MAX_NAME || p[0]=='.' ||
       (n==1&&p[0]=='.') || (n==2&&p[0]=='.'&&p[1]=='.'))return 0;
    memcpy(out,p,n);out[n]=0;
    return 1;
}
static int regular(int dirfd,const char *name,struct stat *s){
    if(fstatat(dirfd,name,s,AT_SYMLINK_NOFOLLOW)!=0)return -1;
    if(!S_ISREG(s->st_mode))return -2;
    return 0;
}
static uint32_t status_errno(int e){return e==ENOENT?QRX_HOSTFS_NOENT:QRX_HOSTFS_IOERR;}
#include "qrx_hostfs_tree.inc"
static void execute(QRXVM *v,QRXHostFS *fs,uint32_t op){
    if(op>=0x101&&op<=0x10a){execute_tree(v,fs,op-0x100);return;}
    fs->status=QRX_HOSTFS_INVALID; fs->result=0;fs->size=0;
    if(op<1||op>QRX_HOSTFS_CMD_TRUNCATE)return;
    if((op==QRX_HOSTFS_CMD_WRITE||op==QRX_HOSTFS_CMD_DELETE||op==QRX_HOSTFS_CMD_TRUNCATE)&&fs->readonly){
        fs->status=QRX_HOSTFS_RDONLY;return;
    }
    if(op!=QRX_HOSTFS_CMD_STAT&&op!=QRX_HOSTFS_CMD_DELETE&&op!=QRX_HOSTFS_CMD_TRUNCATE&&
       (!fs->length||fs->length>QRX_HOSTFS_MAX_TRANSFER||
        !bounds(v,fs->data_dma,fs->length)))return;
    if(op==QRX_HOSTFS_CMD_LIST){
        int fd=dup(fs->dirfd);if(fd<0){fs->status=QRX_HOSTFS_IOERR;return;}
        DIR *d=fdopendir(fd);if(!d){close(fd);fs->status=QRX_HOSTFS_IOERR;return;}
        /* Linux dup shares the directory offset: rewind before scanning. */
        rewinddir(d);
        struct dirent *ent;size_t at=0;
        uint8_t *data=v->ram.data+fs->data_dma;
        while((ent=readdir(d))){
            size_t n=strlen(ent->d_name);
            if(n==0||n>QRX_HOSTFS_MAX_NAME||ent->d_name[0]=='.')continue;
            struct stat st;
            if(regular(fs->dirfd,ent->d_name,&st)!=0)continue;
            int ok=1;
            for(size_t j=0;j<n;j++)if(!(isalnum((unsigned char)ent->d_name[j])||
                ent->d_name[j]=='_'||ent->d_name[j]=='-'||ent->d_name[j]=='.'||ent->d_name[j]==' '))ok=0;
            if(!ok)continue;
            if(n+1>fs->length-at)break;
            memcpy(data+at,ent->d_name,n);at+=n;data[at++]='\n';
        }
        closedir(d);
        fs->result=(uint32_t)at;fs->status=QRX_HOSTFS_OK;
        if(at){v->ram_write_epoch++;if(v->dma_ram_write)v->dma_ram_write(v->dma_opaque,fs->data_dma,(unsigned)at);}
        return;
    }
    char name[QRX_HOSTFS_MAX_NAME+1];
    if(!safe_name(v,fs,name))return;
    struct stat st;int found=regular(fs->dirfd,name,&st);
    if(op==QRX_HOSTFS_CMD_DELETE){
        if(found){fs->status=found==-2?QRX_HOSTFS_ACCESS:status_errno(errno);return;}
        fs->status=unlinkat(fs->dirfd,name,0)==0?QRX_HOSTFS_OK:QRX_HOSTFS_IOERR;
        return;
    }
    if(op==QRX_HOSTFS_CMD_STAT){
        if(found){fs->status=found==-2?QRX_HOSTFS_ACCESS:status_errno(errno);return;}
        if(st.st_size<0||(uint64_t)st.st_size>QRX_HOSTFS_MAX_FILE){fs->status=QRX_HOSTFS_ACCESS;return;}
        fs->size=(uint32_t)st.st_size;fs->status=QRX_HOSTFS_OK;return;
    }
    if(op==QRX_HOSTFS_CMD_READ){
        if(found){fs->status=found==-2?QRX_HOSTFS_ACCESS:status_errno(errno);return;}
        if(st.st_size<0||(uint64_t)st.st_size>QRX_HOSTFS_MAX_FILE){fs->status=QRX_HOSTFS_ACCESS;return;}
        fs->size=(uint32_t)st.st_size;
        if(fs->offset>=fs->size){fs->status=QRX_HOSTFS_OK;return;}
        int fd=openat(fs->dirfd,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
        if(fd<0){fs->status=status_errno(errno);return;}
        if(fstat(fd,&st)!=0||!S_ISREG(st.st_mode)){close(fd);fs->status=QRX_HOSTFS_ACCESS;return;}
        uint32_t limit=fs->length;
        if(limit>fs->size-fs->offset)limit=fs->size-fs->offset;
        ssize_t n=pread(fd,v->ram.data+fs->data_dma,limit,fs->offset);
        close(fd);
        if(n<0){fs->status=QRX_HOSTFS_IOERR;return;}
        fs->result=(uint32_t)n;fs->status=QRX_HOSTFS_OK;
        if(n>0){v->ram_write_epoch++;if(v->dma_ram_write)v->dma_ram_write(v->dma_opaque,fs->data_dma,(unsigned)n);}
        return;
    }
    if(op==QRX_HOSTFS_CMD_TRUNCATE){
        if(found){fs->status=found==-2?QRX_HOSTFS_ACCESS:status_errno(errno);return;}
        if(fs->offset>QRX_HOSTFS_MAX_FILE)return;
        int fd=openat(fs->dirfd,name,O_RDWR|O_NOFOLLOW|O_NONBLOCK);
        if(fd<0){fs->status=status_errno(errno);return;}
        if(fstat(fd,&st)!=0||!S_ISREG(st.st_mode)){
            close(fd);fs->status=QRX_HOSTFS_ACCESS;return;
        }
        int ok=ftruncate(fd,(off_t)fs->offset)==0 && fsync(fd)==0;
        close(fd);
        fs->status=ok?QRX_HOSTFS_OK:QRX_HOSTFS_IOERR;
        fs->size=ok?fs->offset:0;return;
    }
    /* Explicit RW opt-in. Never follow symlinks and never write special files.
     * The regular fd is checked again after open to close the rename race. */
    if(found==-2){fs->status=QRX_HOSTFS_ACCESS;return;}
    if(found==-1 && errno!=ENOENT){fs->status=QRX_HOSTFS_IOERR;return;}
    if((uint64_t)fs->offset+fs->length>QRX_HOSTFS_MAX_FILE)return;
    int fd=openat(fs->dirfd,name,O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK,0600);
    if(fd<0){fs->status=status_errno(errno);return;}
    if(fstat(fd,&st)!=0||!S_ISREG(st.st_mode)||st.st_size<0||
       (uint64_t)st.st_size>QRX_HOSTFS_MAX_FILE){
        close(fd);fs->status=QRX_HOSTFS_ACCESS;return;
    }
    ssize_t n=pwrite(fd,v->ram.data+fs->data_dma,fs->length,fs->offset);
    int err=0;
    if(n!=(ssize_t)fs->length || fsync(fd)!=0)err=1;
    if(fstat(fd,&st)!=0)err=1;
    close(fd);
    if(err){fs->status=QRX_HOSTFS_IOERR;return;}
    fs->result=(uint32_t)n;fs->size=(uint32_t)st.st_size;fs->status=QRX_HOSTFS_OK;
}
bool qrx_hostfs_mount(QRXHostFS *fs,const char *dir,bool readonly){
    if(!fs||!dir||!*dir)return false;
    memset(fs,0,sizeof(*fs));fs->dirfd=-1;
    int fd=open(dir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(fd<0)return false;
    struct stat st;
    if(fstat(fd,&st)!=0||!S_ISDIR(st.st_mode)){close(fd);return false;}
    fs->dirfd=fd;fs->mounted=true;fs->readonly=readonly;return true;
}
void qrx_hostfs_unmount(QRXHostFS *fs){
    if(!fs)return;
    if(fs->mounted && fs->dirfd>=0)close(fs->dirfd);
    memset(fs,0,sizeof(*fs));fs->dirfd=-1;
}
void qrx_hostfs_attach(QRXVM *v,QRXHostFS *fs){if(v)v->hostfs=fs;}
void qrx_hostfs_attach2(QRXVM *v,QRXHostFS *fs){if(v)v->hostfs2=fs;}
static bool addr_ok(QRXVM *v,uint32_t a,unsigned n){return v&&v->hostfs&&v->hostfs->mounted&&n==4&&!(a&3u)&&a>=QRX_HOSTFS_BASE&&a<=QRX_HOSTFS_SIZE;}
static bool addr2_ok(QRXVM *v,uint32_t a,unsigned n){return v&&v->hostfs2&&v->hostfs2->mounted&&n==4&&!(a&3u)&&a>=QRX_HOSTFS2_BASE&&a<=QRX_HOSTFS2_SIZE;}
bool qrx_hostfs_read(QRXVM *v,uint32_t a,unsigned n,uint32_t *out){
    if(!out||!addr_ok(v,a,n))return false;
    QRXHostFS *s=v->hostfs;
    switch(a){
    case QRX_HOSTFS_MAGIC:*out=0x51525846u;return true;
    case QRX_HOSTFS_FEATURES:*out=0x11u+(s->readonly?0u:2u);return true;
    case QRX_HOSTFS_NAME_DMA:*out=s->name_dma;return true;
    case QRX_HOSTFS_DATA_DMA:*out=s->data_dma;return true;
    case QRX_HOSTFS_OFFSET:*out=s->offset;return true;
    case QRX_HOSTFS_LENGTH:*out=s->length;return true;
    case QRX_HOSTFS_STATUS:*out=s->status;return true;
    case QRX_HOSTFS_RESULT:*out=s->result;return true;
    case QRX_HOSTFS_SIZE:*out=s->size;return true;
    default:return false;
    }
}
bool qrx_hostfs_write(QRXVM *v,uint32_t a,unsigned n,uint32_t val){
    if(!addr_ok(v,a,n))return false;
    QRXHostFS *s=v->hostfs;
    switch(a){
    case QRX_HOSTFS_NAME_DMA:s->name_dma=val;return true;
    case QRX_HOSTFS_DATA_DMA:s->data_dma=val;return true;
    case QRX_HOSTFS_OFFSET:s->offset=val;return true;
    case QRX_HOSTFS_LENGTH:s->length=val;return true;
    case QRX_HOSTFS_COMMAND:execute(v,s,val);return true;
    default:return false;
    }
}
bool qrx_hostfs2_read(QRXVM *v,uint32_t a,unsigned n,uint32_t *out){
    if(!out||!addr2_ok(v,a,n))return false;
    QRXHostFS *s=v->hostfs2;
    switch(a){
    case QRX_HOSTFS2_MAGIC:*out=0x51525846u;return true;
    case QRX_HOSTFS2_FEATURES:*out=0x11u+(s->readonly?0u:2u);return true;
    case QRX_HOSTFS2_NAME_DMA:*out=s->name_dma;return true;
    case QRX_HOSTFS2_DATA_DMA:*out=s->data_dma;return true;
    case QRX_HOSTFS2_OFFSET:*out=s->offset;return true;
    case QRX_HOSTFS2_LENGTH:*out=s->length;return true;
    case QRX_HOSTFS2_STATUS:*out=s->status;return true;
    case QRX_HOSTFS2_RESULT:*out=s->result;return true;
    case QRX_HOSTFS2_SIZE:*out=s->size;return true;
    default:return false;
    }
}
bool qrx_hostfs2_write(QRXVM *v,uint32_t a,unsigned n,uint32_t val){
    if(!addr2_ok(v,a,n))return false;
    QRXHostFS *s=v->hostfs2;
    switch(a){
    case QRX_HOSTFS2_NAME_DMA:s->name_dma=val;return true;
    case QRX_HOSTFS2_DATA_DMA:s->data_dma=val;return true;
    case QRX_HOSTFS2_OFFSET:s->offset=val;return true;
    case QRX_HOSTFS2_LENGTH:s->length=val;return true;
    case QRX_HOSTFS2_COMMAND:execute(v,s,val);return true;
    default:return false;
    }
}
