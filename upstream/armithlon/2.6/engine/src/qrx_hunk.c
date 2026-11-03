#include "qrx_hunk.h"
#include <stdlib.h>
#include <string.h>

#define HUNK_HEADER 0x3f3u
#define HUNK_CODE 0x3e9u
#define HUNK_DATA 0x3eau
#define HUNK_BSS 0x3ebu
#define HUNK_RELOC32 0x3ecu
#define HUNK_RELOC32SHORT 0x3fcu
#define HUNK_SYMBOL 0x3f0u
#define HUNK_DEBUG 0x3f1u
#define HUNK_END 0x3f2u

typedef struct {const uint8_t *src;size_t len,pos;} Reader;
static bool take(Reader *r,uint32_t *v){
 if(!r||!v||r->pos>r->len||r->len-r->pos<4)return false;
 const uint8_t *p=r->src+r->pos;
 *v=((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
 r->pos+=4;return true;
}
static bool take16(Reader *r,uint16_t *v){
 if(!r||!v||r->pos>r->len||r->len-r->pos<2)return false;
 *v=((uint16_t)r->src[r->pos]<<8)|r->src[r->pos+1];r->pos+=2;return true;
}
static uint32_t b32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void w32(uint8_t *p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
static bool skip_words(Reader *r,uint32_t count){
 if(count>(SIZE_MAX-r->pos)/4u || (size_t)count*4u>r->len-r->pos)return false;
 r->pos+=(size_t)count*4u;return true;
}
/* Allocate in a scratch buffer first so parse failures never change RAM. */
bool qrx_hunk_load(const uint8_t *file,size_t len,uint8_t *guest,size_t guest_len,
                   uint32_t base,uint32_t *entry,size_t *loaded){
 if(!file||!guest||!entry||!loaded||len<20||!base||(base&3)||
    (uint64_t)base+4u>guest_len)return false;
 Reader r={file,len,0};uint32_t v=0;
 if(!take(&r,&v)||v!=HUNK_HEADER)return false;
 for(;;){if(!take(&r,&v))return false;if(!v)break;if(!skip_words(&r,v))return false;}
 uint32_t table=0,first=0,last=0;
 if(!take(&r,&table)||!take(&r,&first)||!take(&r,&last)||
    !table||table>QRX_HUNK_MAX_SEGMENTS||first>last||
    (uint64_t)last-first+1u!=table)return false;
 uint32_t addr[QRX_HUNK_MAX_SEGMENTS],size[QRX_HUNK_MAX_SEGMENTS];
 size_t total=0;
 for(uint32_t i=0;i<table;i++){
    if(!take(&r,&v)|| (v&0xc0000000u))return false; /* reject CHIP/FAST allocation flags until implemented */
    uint64_t segment=(uint64_t)v*4u;
    if(segment>QRX_HUNK_STAGE_MAX || total+segment>QRX_HUNK_STAGE_MAX ||
       (uint64_t)base+total+segment>guest_len)return false;
    addr[i]=base+(uint32_t)total;size[i]=(uint32_t)segment;total+=(size_t)segment;
 }
 uint8_t *scratch=calloc(1,total?total:1);
 if(!scratch)return false;
 bool good=false;
 for(uint32_t i=0;i<table;i++){
    uint32_t type=0,words=0;size_t position=(size_t)(addr[i]-base);
    if(!take(&r,&type)|| (type!=HUNK_CODE && type!=HUNK_DATA && type!=HUNK_BSS))goto done;
    if(i==0 && type!=HUNK_CODE)goto done;
    if(!take(&r,&words)||words>size[i]/4u)goto done;
    if(type==HUNK_BSS){if(words*4u!=size[i])goto done;}
    else {
       if(!skip_words(&r,words))goto done;
       memcpy(scratch+position,r.src+r.pos-(size_t)words*4u,(size_t)words*4u);
    }
    for(;;){
        if(!take(&r,&v))goto done;
        if(v==HUNK_END)break;
        if(v==HUNK_RELOC32){
            for(;;){
                uint32_t count=0,reference=0;
                if(!take(&r,&count))goto done;
                if(!count)break;
                if(!take(&r,&reference)||reference<first||reference>last||count>QRX_HUNK_STAGE_MAX/4u)goto done;
                for(uint32_t j=0;j<count;j++){
                    uint32_t off=0;if(!take(&r,&off)||(off&1u)||off>size[i]||size[i]-off<4u)goto done;
                    uint8_t *at=scratch+position+off;
                    uint64_t sum=(uint64_t)b32(at)+addr[reference-first];
                    if(sum>UINT32_MAX)goto done;
                    w32(at,(uint32_t)sum);
                }
            }
        }else if(v==HUNK_RELOC32SHORT){
            /* Real Amiga HUNK_RELOC32SHORT: u16 count, u16 hunk,
               u16 offsets... repeated until count==0, longword padding. */
            for(;;){
                uint16_t count=0,reference=0;
                if(!take16(&r,&count))goto done;
                if(!count)break;
                if(!take16(&r,&reference)||reference<first||reference>last||count>QRX_HUNK_STAGE_MAX/2u)goto done;
                for(unsigned j=0;j<count;j++){
                    uint16_t off=0;
                    if(!take16(&r,&off)||(off&1u)||off>size[i]||size[i]-off<4u)goto done;
                    uint8_t *at=scratch+position+off;
                    uint64_t sum=(uint64_t)b32(at)+addr[reference-first];
                    if(sum>UINT32_MAX)goto done;
                    w32(at,(uint32_t)sum);
                }
            }
            /* Short relocations are rounded to the next longword. */
            if(r.pos&2u){uint16_t pad=0;if(!take16(&r,&pad)||pad)goto done;}
        }else if(v==HUNK_SYMBOL){
            for(;;){uint32_t words_name=0;
                if(!take(&r,&words_name))goto done;
                if(!words_name)break;
                if(!skip_words(&r,words_name)||!take(&r,&words_name))goto done;
            }
        }else if(v==HUNK_DEBUG){uint32_t words_debug=0;
            if(!take(&r,&words_debug)||!skip_words(&r,words_debug))goto done;
        }else goto done;
    }
 }
 if(r.pos!=r.len || !size[0])goto done;
 memcpy(guest+base,scratch,total);
 *entry=addr[0];*loaded=total;good=true;
done:
 free(scratch);return good;
}
