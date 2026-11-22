/* Poll the host event FIFO and deliver real input.device events. */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <intuition/screens.h>
#include <intuition/intuitionbase.h>
struct IntuitionBase *IntuitionBase;
#include <devices/input.h>
#include <devices/inputevent.h>
#include <devices/timer.h>
#include <string.h>
#include <stdio.h>
static ULONG regread(unsigned offset){ULONG v;volatile ULONG*p=(volatile ULONG*)(0xff000300u+offset);__asm__ volatile("move.l (%1),%0":"=d"(v):"a"(p):"memory");return v;}
#define POLL_US 2000
int main(void){
 IntuitionBase=(void*)OpenLibrary("intuition.library",39);if(!IntuitionBase)return 20;
 struct MsgPort*port=CreateMsgPort();if(!port)return 20;
 struct IOStdReq*io=(void*)CreateIORequest(port,sizeof(*io));if(!io)return 20;
 if(OpenDevice("input.device",0,(struct IORequest*)io,0))return 20;
 puts("QRX host input ready");fflush(stdout);
 struct MsgPort*tport=CreateMsgPort();
 struct timerequest*tr=tport?(void*)CreateIORequest(tport,sizeof(*tr)):NULL;
 int timer_open=tr&&!OpenDevice(TIMERNAME,UNIT_MICROHZ,(struct IORequest*)tr,0);
 UWORD qualifiers=0;
 ULONG next=0;int have_next=0;
 for(;;){
  while(have_next||regread(0)){
   ULONG event=have_next?next:regread(4);have_next=0;unsigned type=event>>24,code=(event>>16)&255;
   /* Several positions in a row: only the newest counts, the pointer must not trail. */
   while(type==3&&regread(0)){next=regread(4);if((next>>24)==3)event=next;else{have_next=1;break;}}
   struct IEPointerPixel pixel={0};
   struct InputEvent ie;memset(&ie,0,sizeof ie);ie.ie_Qualifier=qualifiers;
   if(type==1||type==2){
    ie.ie_Class=IECLASS_RAWKEY;ie.ie_Code=code|(type==2?IECODE_UP_PREFIX:0);
    unsigned bit=code==0x60?IEQUALIFIER_LSHIFT:code==0x61?IEQUALIFIER_RSHIFT:code==0x62?IEQUALIFIER_CAPSLOCK:code==0x63?IEQUALIFIER_CONTROL:code==0x64?IEQUALIFIER_LALT:code==0x65?IEQUALIFIER_RALT:code==0x66?IEQUALIFIER_LCOMMAND:code==0x67?IEQUALIFIER_RCOMMAND:0;
    if(type==1)qualifiers|=bit;else qualifiers&=~bit;ie.ie_Qualifier=qualifiers;
    if(type==1&&(event&1))ie.ie_Qualifier|=IEQUALIFIER_REPEAT;
   }else if(type==3){pixel.iepp_Screen=LockPubScreen("QRX");if(!pixel.iepp_Screen){puts("QRX screen lock failed");fflush(stdout);continue;}ie.ie_Class=IECLASS_NEWPOINTERPOS;ie.ie_SubClass=IESUBCLASS_PIXEL;ie.ie_Code=IECODE_NOBUTTON;pixel.iepp_Position.X=(event>>12)&4095u;pixel.iepp_Position.Y=event&4095u;ie.ie_EventAddress=&pixel;
   }else if(type==4){ie.ie_Class=IECLASS_RAWMOUSE;unsigned button=code==0?IECODE_LBUTTON:code==1?IECODE_RBUTTON:IECODE_MBUTTON;
    unsigned bit=code==0?IEQUALIFIER_LEFTBUTTON:code==1?IEQUALIFIER_RBUTTON:IEQUALIFIER_MIDBUTTON;
    if(event&1)qualifiers|=bit;else qualifiers&=~bit;
    ie.ie_Code=button|((event&1)?0:IECODE_UP_PREFIX);ie.ie_Qualifier=qualifiers|IEQUALIFIER_RELATIVEMOUSE;
   }else continue;
   io->io_Command=type==4?IND_ADDEVENT:IND_WRITEEVENT;io->io_Data=&ie;io->io_Length=sizeof ie;LONG error=DoIO((struct IORequest*)io);
   if(pixel.iepp_Screen)UnlockPubScreen(NULL,pixel.iepp_Screen);
   if(error){printf("input error %ld\n",error);fflush(stdout);}
  }
  /* Delay(1) would wait for a 50 Hz tick, and often the one after: 30 updates a second
   * at best, with keys arriving in bursts. */
  if(timer_open){tr->tr_node.io_Command=TR_ADDREQUEST;tr->tr_time.tv_secs=0;tr->tr_time.tv_micro=POLL_US;DoIO((struct IORequest*)tr);}
  else Delay(1);
 }
}
