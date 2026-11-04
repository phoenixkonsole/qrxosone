/* Hierarchical ABI v2, opcodes 0x101..0x10a. Existing flat ABI is unchanged.
 * Every component is opened relative to the root capability, without symlinks. */
#define HFS_PATH_MAX 1023u
static int tree_string(QRXVM *v,uint32_t ptr,char *out){
    for(unsigned i=0;i<=HFS_PATH_MAX;i++){
        if(!bounds(v,ptr+i,1))return 0;
        unsigned ch=v->ram.data[ptr+i];out[i]=(char)ch;
        if(!ch)return 1;
        if(ch<32 || ch==127 || ch==':' || ch=='\\')return 0;
    }
    return 0;
}
static int tree_component(int fd,const char *wanted,char *actual){
    struct stat st;
    if(fstatat(fd,wanted,&st,AT_SYMLINK_NOFOLLOW)==0){strcpy(actual,wanted);return 1;}
#if defined(__APPLE__) && defined(_PC_CASE_SENSITIVE)
    /* On a case-insensitive volume the failed lookup already was the answer;
     * AROS probes many absent names, and a scan per probe is expensive. */
    if(errno==ENOENT&&fpathconf(fd,_PC_CASE_SENSITIVE)==0){strcpy(actual,wanted);return 1;}
#endif
    int copy=openat(fd,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(copy<0)return 0;
    DIR *d=fdopendir(copy);if(!d){close(copy);return 0;}
    struct dirent *e;int found=0;
    while((e=readdir(d)))if(!strcasecmp(e->d_name,wanted)){
        if(found){found=-1;break;} /* ambiguous case-folding */
        strcpy(actual,e->d_name);found=1;
    }
    closedir(d);
    if(!found)strcpy(actual,wanted);
    return found>=0;
}
static int tree_parent(QRXHostFS *s,const char *path,char leaf[256]){
    if(*path=='/'){errno=EACCES;return -1;}
    int fd=openat(s->dirfd,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(fd<0)return -1;
    const char *p=path;
    if(!*p){strcpy(leaf,".");return fd;}
    for(;;){
        const char *slash=strchr(p,'/');size_t n=slash?(size_t)(slash-p):strlen(p);
        if(!n||n>255||(n==1&&p[0]=='.')||(n==2&&p[0]=='.'&&p[1]=='.')){errno=EACCES;break;}
        char component[256];memcpy(component,p,n);component[n]=0;
        if(!tree_component(fd,component,leaf)){errno=EACCES;break;}
        if(!slash)return fd;
        int next=openat(fd,leaf,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
        if(next<0)break;
        close(fd);fd=next;p=slash+1;
    }
    int saved=errno;close(fd);errno=saved;return -1;
}
/* Durability: guest writes reach the host's page cache at once, so a crash of
 * the emulator or guest loses nothing. A background thread then forces them onto
 * the disk within HFS_FLUSH_MS, which covers a kernel panic or power cut too,
 * without stalling the guest on every 4 KiB packet. */
#define HFS_FLUSH_MS 200u
#define HFS_FLUSH_QUEUE 64u
static pthread_mutex_t hfs_flush_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hfs_flush_idle=PTHREAD_COND_INITIALIZER;
static int hfs_flush_fds[HFS_FLUSH_QUEUE];
static unsigned hfs_flush_count,hfs_flush_busy;
static uint64_t hfs_flush_cycle=1;
static bool hfs_flush_started;
static void hfs_full_sync(int fd){
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    /* Plain fsync on macOS stops at the drive's volatile cache. */
    if(fcntl(fd,F_FULLFSYNC)==0)return;
#endif
    fsync(fd);
}
static void *hfs_flush_thread(void *arg){
    (void)arg;
    for(;;){
        struct timespec nap={0,(long)HFS_FLUSH_MS*1000000L};
        nanosleep(&nap,NULL);
        int batch[HFS_FLUSH_QUEUE];
        pthread_mutex_lock(&hfs_flush_lock);
        unsigned n=hfs_flush_count;
        memcpy(batch,hfs_flush_fds,n*sizeof(int));
        hfs_flush_count=0;hfs_flush_busy=n;hfs_flush_cycle++;
        pthread_mutex_unlock(&hfs_flush_lock);
        for(unsigned i=0;i<n;i++){hfs_full_sync(batch[i]);close(batch[i]);}
        pthread_mutex_lock(&hfs_flush_lock);
        hfs_flush_busy=0;
        pthread_cond_broadcast(&hfs_flush_idle);
        pthread_mutex_unlock(&hfs_flush_lock);
    }
    return NULL;
}
/* Queues one duplicate of fd per flush cycle; a full queue syncs right here. */
static void hfs_flush_request(int fd,uint64_t *seen_cycle){
    pthread_mutex_lock(&hfs_flush_lock);
    if(!hfs_flush_started){
        pthread_t tid;
        if(pthread_create(&tid,NULL,hfs_flush_thread,NULL)==0){pthread_detach(tid);hfs_flush_started=true;}
    }
    if(seen_cycle&&*seen_cycle==hfs_flush_cycle){pthread_mutex_unlock(&hfs_flush_lock);return;}
    bool queued=false;
    if(hfs_flush_started&&hfs_flush_count<HFS_FLUSH_QUEUE){
        int copy=dup(fd);
        if(copy>=0){hfs_flush_fds[hfs_flush_count++]=copy;queued=true;}
    }
    if(queued&&seen_cycle)*seen_cycle=hfs_flush_cycle;
    pthread_mutex_unlock(&hfs_flush_lock);
    if(!queued)hfs_full_sync(fd);
}
void qrx_hostfs_sync(void){
    pthread_mutex_lock(&hfs_flush_lock);
    while(hfs_flush_started&&(hfs_flush_count||hfs_flush_busy))pthread_cond_wait(&hfs_flush_idle,&hfs_flush_lock);
    pthread_mutex_unlock(&hfs_flush_lock);
}
unsigned qrx_hostfs_flush_pending(void){
    pthread_mutex_lock(&hfs_flush_lock);
    unsigned n=hfs_flush_count+hfs_flush_busy;
    pthread_mutex_unlock(&hfs_flush_lock);
    return n;
}
/* AmigaDOS reads files in small packets and walks directories by index, and
 * each request used to reopen every path component. These caches serve repeat
 * requests with one syscall. Entries expire quickly because other host programs
 * may change the share too; every guest-side change drops them at once. */
#define HFS_CACHE_TTL_MS 1500u
#define HFS_FILE_SLOTS 16u
typedef struct { char path[HFS_PATH_MAX+1]; int fd; bool writable; uint64_t flush_cycle, opened_ms, used; } HFSFileSlot;
typedef struct { char name[256]; uint32_t size; bool dir; } HFSDirEntry;
struct QRXHostFSCache {
    HFSFileSlot file[HFS_FILE_SLOTS];
    uint64_t clock;
    char dir_path[HFS_PATH_MAX+1];
    HFSDirEntry *dir;
    unsigned dir_count, dir_cap;
    uint64_t dir_ms;
    bool dir_valid;
};
static uint64_t hfs_now_ms(void){
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;
}
static struct QRXHostFSCache *hfs_cache(QRXHostFS *s){
    if(!s->cache){
        s->cache=calloc(1,sizeof(*s->cache));
        if(s->cache)for(unsigned i=0;i<HFS_FILE_SLOTS;i++)s->cache->file[i].fd=-1;
    }
    return s->cache;
}
static void hfs_slot_close(HFSFileSlot *f){
    if(f->fd>=0)close(f->fd);   /* any write already has its flush queued */
    f->fd=-1;f->path[0]=0;f->flush_cycle=0;
}
static void hfs_cache_drop(QRXHostFS *s){
    struct QRXHostFSCache *c=s->cache;
    if(!c)return;
    for(unsigned i=0;i<HFS_FILE_SLOTS;i++)hfs_slot_close(&c->file[i]);
    c->dir_valid=false;
}
static void hfs_cache_free(QRXHostFS *s){
    if(!s->cache)return;
    hfs_cache_drop(s);
    free(s->cache->dir);
    free(s->cache);s->cache=NULL;
}
static HFSFileSlot *hfs_file_find(struct QRXHostFSCache *c,const char *path,bool write){
    uint64_t now=hfs_now_ms();
    for(unsigned i=0;i<HFS_FILE_SLOTS;i++){
        HFSFileSlot *f=&c->file[i];
        if(f->fd<0||strcmp(f->path,path))continue;
        if(now-f->opened_ms>HFS_CACHE_TTL_MS||(write&&!f->writable)){hfs_slot_close(f);return NULL;}
        f->used=++c->clock;
        return f;
    }
    return NULL;
}
static HFSFileSlot *hfs_file_insert(struct QRXHostFSCache *c,const char *path,int fd,bool write){
    HFSFileSlot *best=&c->file[0];
    for(unsigned i=0;i<HFS_FILE_SLOTS;i++){
        if(c->file[i].fd<0){best=&c->file[i];break;}
        if(c->file[i].used<best->used)best=&c->file[i];
    }
    hfs_slot_close(best);
    strcpy(best->path,path);best->fd=fd;best->writable=write;best->flush_cycle=0;
    best->opened_ms=hfs_now_ms();best->used=++c->clock;
    return best;
}
/* Enumerates once per listing; the guest's index then needs no directory scan. */
static int hfs_dir_load(struct QRXHostFSCache *c,int parent,const char *leaf,const char *path){
    if(c->dir_valid&&!strcmp(c->dir_path,path)&&hfs_now_ms()-c->dir_ms<=HFS_CACHE_TTL_MS)return 0;
    c->dir_valid=false;c->dir_count=0;
    int fd=openat(parent,leaf,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(fd<0)return -1;
    DIR *dir=fdopendir(fd);
    if(!dir){int e=errno;close(fd);errno=e;return -1;}
    struct dirent *e;struct stat st;
    while((e=readdir(dir))){
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        if(fstatat(dirfd(dir),e->d_name,&st,AT_SYMLINK_NOFOLLOW)||(!S_ISREG(st.st_mode)&&!S_ISDIR(st.st_mode)))continue;
        if(c->dir_count==c->dir_cap){
            unsigned cap=c->dir_cap?c->dir_cap*2u:64u;
            HFSDirEntry *grown=realloc(c->dir,cap*sizeof(*grown));
            if(!grown){closedir(dir);errno=ENOMEM;return -1;}
            c->dir=grown;c->dir_cap=cap;
        }
        HFSDirEntry *d=&c->dir[c->dir_count++];
        strncpy(d->name,e->d_name,sizeof(d->name)-1);d->name[sizeof(d->name)-1]=0;
        d->dir=S_ISDIR(st.st_mode)!=0;d->size=d->dir?0:(uint32_t)st.st_size;
    }
    closedir(dir);
    strcpy(c->dir_path,path);c->dir_ms=hfs_now_ms();c->dir_valid=true;
    return 0;
}
static void tree_changed(QRXVM *v,QRXHostFS *s,unsigned n){
    if(n){v->ram_write_epoch++;if(v->dma_ram_write)v->dma_ram_write(v->dma_opaque,s->data_dma,n);}
}
static void execute_tree(QRXVM *v,QRXHostFS *s,unsigned op){
    s->status=QRX_HOSTFS_INVALID;s->result=0;s->size=0;
    if(op<1||op>10)return;
    if(op>=4 && op<=9 && s->readonly){s->status=QRX_HOSTFS_RDONLY;return;}
    if(op==10){ /* capacity in 64 KiB blocks, within signed AmigaDOS limits */
        struct statvfs space;
        if(fstatvfs(s->dirfd,&space)){s->status=status_errno(errno);return;}
        uint64_t total=(uint64_t)space.f_blocks*space.f_frsize/65536;
        uint64_t used=(uint64_t)(space.f_blocks-space.f_bfree)*space.f_frsize/65536;
        s->size=(uint32_t)(total>INT32_MAX?INT32_MAX:total);
        s->result=(uint32_t)(used>s->size?s->size:used);s->status=0;return;
    }
    char path[HFS_PATH_MAX+1],leaf[256];
    if(!tree_string(v,s->name_dma,path))return;
    static int trace=-1;if(trace<0)trace=getenv("QRX_HOSTFS_TRACE")!=NULL;
    if(trace)fprintf(stderr,"[HOSTFS] op=%u path=%s offset=%u length=%u\n",op,path,s->offset,s->length);
    struct QRXHostFSCache *cache=hfs_cache(s);
    /* Everything that changes the tree invalidates; reads and writes of an
     * already-open file stay cached. */
    if(cache&&(op==5||op==6||op==7||op==8||op==9))hfs_cache_drop(s);
    if(cache&&op==4)cache->dir_valid=false;
    if(cache&&(op==3||op==4)){
        if(s->length>QRX_HOSTFS_MAX_TRANSFER||(!bounds(v,s->data_dma,s->length)&&s->length))return;
        if((uint64_t)s->offset+s->length>QRX_HOSTFS_MAX_FILE)return;
        HFSFileSlot *f=hfs_file_find(cache,path,op==4);
        if(f){
            ssize_t got=op==3?pread(f->fd,v->ram.data+s->data_dma,s->length,s->offset)
                             :pwrite(f->fd,v->ram.data+s->data_dma,s->length,s->offset);
            struct stat fst;
            if(got<0||fstat(f->fd,&fst)){s->status=status_errno(errno);return;}
            if(op==4)hfs_flush_request(f->fd,&f->flush_cycle);
            s->size=(uint32_t)fst.st_size;s->result=(uint32_t)got;s->status=0;
            if(op==3)tree_changed(v,s,(unsigned)got);
            return;
        }
    }
    int parent=tree_parent(s,path,leaf);
    if(parent<0){s->status=errno==ENOENT?QRX_HOSTFS_NOENT:QRX_HOSTFS_ACCESS;return;}
    struct stat st;int fd=-1;ssize_t n=-1;
    if(op==1){ /* one directory entry; offset is the enumeration index */
        if(!s->length||s->length>QRX_HOSTFS_MAX_TRANSFER||!bounds(v,s->data_dma,s->length))goto done;
        if(!cache){s->status=QRX_HOSTFS_IOERR;goto done;}
        if(hfs_dir_load(cache,parent,leaf,path))goto io_error;
        s->status=QRX_HOSTFS_NOENT;
        if(s->offset<cache->dir_count){
            const HFSDirEntry *e=&cache->dir[s->offset];
            size_t len=strlen(e->name)+1;
            if(len>s->length){s->status=QRX_HOSTFS_INVALID;goto done;}
            memcpy(v->ram.data+s->data_dma,e->name,len);
            s->result=(uint32_t)len|(e->dir?0x80000000u:0);
            s->size=e->size;
            s->status=0;tree_changed(v,s,(unsigned)len);
        }
        goto done;
    }
    if(op==2){
        if(fstatat(parent,leaf,&st,AT_SYMLINK_NOFOLLOW))goto io_error;
        if(!S_ISREG(st.st_mode)&&!S_ISDIR(st.st_mode)){s->status=QRX_HOSTFS_ACCESS;goto done;}
        s->result=S_ISDIR(st.st_mode);s->size=S_ISDIR(st.st_mode)?0:(uint32_t)st.st_size;s->status=0;goto done;
    }
    if(!*path){s->status=QRX_HOSTFS_ACCESS;goto done;}
    if(op==5){
        if(fstatat(parent,leaf,&st,AT_SYMLINK_NOFOLLOW))goto io_error;
        if(!S_ISREG(st.st_mode)&&!S_ISDIR(st.st_mode)){s->status=QRX_HOSTFS_ACCESS;goto done;}
        if(unlinkat(parent,leaf,S_ISDIR(st.st_mode)?AT_REMOVEDIR:0))goto io_error;
        hfs_full_sync(parent);
        s->status=0;goto done;
    }
    if(op==7){if(mkdirat(parent,leaf,0700))goto io_error;hfs_full_sync(parent);s->status=0;goto done;}
    if(op==8){
        if(fstatat(parent,leaf,&st,AT_SYMLINK_NOFOLLOW))goto io_error;
        if(!S_ISREG(st.st_mode)&&!S_ISDIR(st.st_mode)){s->status=QRX_HOSTFS_ACCESS;goto done;}
        char dest[HFS_PATH_MAX+1],other[256];
        if(!tree_string(v,s->data_dma,dest)||!*dest)goto done;
        int target=tree_parent(s,dest,other);if(target<0){s->status=QRX_HOSTFS_ACCESS;goto done;}
        if(fstatat(target,other,&st,AT_SYMLINK_NOFOLLOW)==0){close(target);s->status=QRX_HOSTFS_ACCESS;goto done;}
        int result=renameat(parent,leaf,target,other);
        if(!result){hfs_full_sync(target);hfs_full_sync(parent);}
        close(target);
        if(result)goto io_error;s->status=0;goto done;
    }
    if(op==3||op==4){
        if(s->length>QRX_HOSTFS_MAX_TRANSFER||(!bounds(v,s->data_dma,s->length)&&s->length))goto done;
        if((uint64_t)s->offset+s->length>QRX_HOSTFS_MAX_FILE)goto done;
    }
    if(op==6 && s->offset>QRX_HOSTFS_MAX_FILE)goto done;
    fd=openat(parent,leaf,(op==3?O_RDONLY:O_RDWR)|(op==9?O_CREAT:0)|O_NOFOLLOW|O_NONBLOCK,0600);
    if(fd<0)goto io_error;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)){s->status=QRX_HOSTFS_ACCESS;goto done;}
    if(op==3)n=pread(fd,v->ram.data+s->data_dma,s->length,s->offset);
    if(op==4)n=pwrite(fd,v->ram.data+s->data_dma,s->length,s->offset);
    if(op==6||op==9)n=ftruncate(fd,op==9?0:s->offset);
    if(n<0)goto io_error;
    /* A create or truncate changes the file's shape: on disk before the reply,
     * like the directory entry. Plain writes go through the flush thread. */
    if(op==6||op==9){hfs_full_sync(fd);hfs_full_sync(parent);}
    if(fstat(fd,&st))goto io_error;
    s->size=(uint32_t)st.st_size;s->result=(uint32_t)n;s->status=0;
    if(op==3)tree_changed(v,s,(unsigned)n);
    if(cache&&(op==3||op==4)){
        HFSFileSlot *slot=hfs_file_insert(cache,path,fd,op==4);
        if(op==4)hfs_flush_request(slot->fd,&slot->flush_cycle);
        fd=-1;
    }else if(op==4)hfs_full_sync(fd);
    goto done;
io_error:s->status=status_errno(errno);
done:if(trace)fprintf(stderr,"[HOSTFS] status=%u result=%u size=%u\n",s->status,s->result,s->size);if(fd>=0)close(fd);close(parent);
}
