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
    int parent=tree_parent(s,path,leaf);
    if(parent<0){s->status=errno==ENOENT?QRX_HOSTFS_NOENT:QRX_HOSTFS_ACCESS;return;}
    struct stat st;int fd=-1;ssize_t n=-1;
    if(op==1){ /* one directory entry; offset is the enumeration index */
        if(!s->length||s->length>QRX_HOSTFS_MAX_TRANSFER||!bounds(v,s->data_dma,s->length))goto done;
        fd=openat(parent,leaf,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
        if(fd<0)goto io_error;
        DIR *dir=fdopendir(fd);if(!dir)goto io_error;fd=-1;
        struct dirent *e;unsigned index=0;
        s->status=QRX_HOSTFS_NOENT;
        while((e=readdir(dir))){
            if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
            if(fstatat(dirfd(dir),e->d_name,&st,AT_SYMLINK_NOFOLLOW)||(!S_ISREG(st.st_mode)&&!S_ISDIR(st.st_mode)))continue;
            if(index++!=s->offset)continue;
            size_t len=strlen(e->d_name)+1;
            if(len>s->length){s->status=QRX_HOSTFS_INVALID;break;}
            memcpy(v->ram.data+s->data_dma,e->d_name,len);
            s->result=(uint32_t)len|(S_ISDIR(st.st_mode)?0x80000000u:0);
            s->size=S_ISDIR(st.st_mode)?0:(uint32_t)st.st_size;
            s->status=0;tree_changed(v,s,(unsigned)len);break;
        }
        closedir(dir);goto done;
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
        s->status=0;goto done;
    }
    if(op==7){if(mkdirat(parent,leaf,0700))goto io_error;s->status=0;goto done;}
    if(op==8){
        if(fstatat(parent,leaf,&st,AT_SYMLINK_NOFOLLOW))goto io_error;
        if(!S_ISREG(st.st_mode)&&!S_ISDIR(st.st_mode)){s->status=QRX_HOSTFS_ACCESS;goto done;}
        char dest[HFS_PATH_MAX+1],other[256];
        if(!tree_string(v,s->data_dma,dest)||!*dest)goto done;
        int target=tree_parent(s,dest,other);if(target<0){s->status=QRX_HOSTFS_ACCESS;goto done;}
        if(fstatat(target,other,&st,AT_SYMLINK_NOFOLLOW)==0){close(target);s->status=QRX_HOSTFS_ACCESS;goto done;}
        int result=renameat(parent,leaf,target,other);close(target);
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
    if(op!=3&&fsync(fd))goto io_error;
    if(fstat(fd,&st))goto io_error;
    s->size=(uint32_t)st.st_size;s->result=(uint32_t)n;s->status=0;
    if(op==3)tree_changed(v,s,(unsigned)n);
    goto done;
io_error:s->status=status_errno(errno);
done:if(trace)fprintf(stderr,"[HOSTFS] status=%u result=%u size=%u\n",s->status,s->result,s->size);if(fd>=0)close(fd);close(parent);
}
