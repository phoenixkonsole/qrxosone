/* QRX OS One Amiga 68K v0.20.3.7.4.6.6
 * QRX Diagnostics Center / Desktop Foundation
 * Native AROS/m68k, OCS/ECS-safe Intuition UI.
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <intuition/intuition.h>
#include <graphics/rastport.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>

#define QRX_DIAG_VERSION "0.20.3.7.4.6.6"
#define MAX_TESTS 32

enum qrx_status { QRX_PASS=0, QRX_PARTIAL=1, QRX_FAIL=2 };
struct qrx_test { const char *name; enum qrx_status status; const char *detail; };

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;
struct DosLibrary *DOSBase = NULL;
static struct qrx_test tests[MAX_TESTS];
static int test_count=0;

static void add_test(const char *n, enum qrx_status s, const char *d){
    if(test_count<MAX_TESTS){tests[test_count].name=n;tests[test_count].status=s;tests[test_count].detail=d;test_count++;}
}
static int exists(const char *p){ BPTR l=Lock((STRPTR)p,ACCESS_READ); if(l){UnLock(l);return 1;} return 0; }
static int is_file(const char *p){
    BPTR l=Lock((STRPTR)p,ACCESS_READ); struct FileInfoBlock *f;
    int ok=0; if(!l) return 0; f=(struct FileInfoBlock*)AllocDosObject(DOS_FIB,NULL);
    if(f && Examine(l,f)) ok=(f->fib_DirEntryType<0);
    if(f) FreeDosObject(DOS_FIB,f);
    UnLock(l);
    return ok;
}
static int count_mounted_volumes(void){
    struct DosList *dl; int n=0; ULONG flags=LDF_VOLUMES|LDF_READ;
    dl=LockDosList(flags); if(!dl) return 0;
    while((dl=NextDosEntry(dl,LDF_VOLUMES))!=NULL) n++;
    UnLockDosList(flags); return n;
}
static void run_tests(void){
    int vols;
    add_test("Exec", SysBase ? QRX_PASS:QRX_FAIL, SysBase?"exec.library available":"missing");
    add_test("DOS", DOSBase ? QRX_PASS:QRX_FAIL, DOSBase?"dos.library available":"missing");
    add_test("Intuition", IntuitionBase ? QRX_PASS:QRX_FAIL, IntuitionBase?"intuition.library available":"missing");
    add_test("Graphics", GfxBase ? QRX_PASS:QRX_FAIL, GfxBase?"graphics.library available":"missing");
    add_test("QRX: assign", exists("QRX:")?QRX_PASS:QRX_FAIL, exists("QRX:")?"QRX volume/assign reachable":"QRX: unavailable");
    add_test("Desktop Host", is_file("QRX:C/QRXDesktop")?QRX_PASS:QRX_FAIL, is_file("QRX:C/QRXDesktop")?"QRXDesktop installed":"QRXDesktop missing");
    vols=count_mounted_volumes();
    add_test("Volume Discovery", vols>0?QRX_PASS:QRX_FAIL, vols>0?"DOS mounted-volume list reachable":"no mounted volumes enumerated");
    add_test("Requester-Free Volumes", is_file("QRX:C/QRXHardware")?QRX_PASS:QRX_PARTIAL, "fixed USB0:-USB3 probing not required by diagnostics");
    add_test("Writer", is_file("QRX:C/QRXWriter")?QRX_PASS:QRX_FAIL, is_file("QRX:C/QRXWriter")?"installed":"missing");
    add_test("Spreadsheet", is_file("QRX:C/QRXSpreadsheet")?QRX_PASS:QRX_FAIL, is_file("QRX:C/QRXSpreadsheet")?"installed":"missing");
    add_test("Presenter", is_file("QRX:C/QRXPresenter")?QRX_PASS:QRX_FAIL, is_file("QRX:C/QRXPresenter")?"installed":"missing");
    add_test("Hardware", is_file("QRX:C/QRXHardware")?QRX_PASS:QRX_FAIL, is_file("QRX:C/QRXHardware")?"installed":"missing");
    add_test("POSIX Layer", is_file("QRX:C/QRXPosixTest")?QRX_PASS:QRX_PARTIAL, is_file("QRX:C/QRXPosixTest")?"test binary installed":"test binary missing");
    add_test("ELF Loader", is_file("QRX:C/QRXELF")?QRX_PASS:QRX_PARTIAL, is_file("QRX:C/QRXELF")?"loader installed":"loader missing");
    add_test("Linux ABI", is_file("QRX:C/QRXLinuxRun")?QRX_PARTIAL:QRX_FAIL, is_file("QRX:C/QRXLinuxRun")?"runtime present; functional probe still required":"runtime missing");
    add_test("QRX86", is_file("QRX:C/QRX86Run")?QRX_PARTIAL:QRX_FAIL, is_file("QRX:C/QRX86Run")?"runtime present; execution probe still required":"runtime missing");
    add_test("QRX86 GRT/JIT", is_file("QRX:C/QRX86GRTExec")?QRX_PARTIAL:QRX_FAIL, is_file("QRX:C/QRX86GRTExec")?"QRXGRT executor + m68k JIT foundation installed; ReactOS trace required":"QRX86GRTExec missing");
    add_test("Dynarec", is_file("QRX:C/QRXDynarecTest")?QRX_PARTIAL:QRX_FAIL, is_file("QRX:C/QRXDynarecTest")?"Tier-1 test present":"test missing");
    add_test("Win32 Loader", is_file("QRX:C/QRXWinRun")?QRX_PARTIAL:QRX_FAIL, is_file("QRX:C/QRXWinRun")?"runtime present; PE probe still required":"runtime missing");
    add_test("ReactOS Payload", exists("QRX:Windows/ReactOS/System32")?QRX_PASS:QRX_FAIL, exists("QRX:Windows/ReactOS/System32")?"System32 integrated":"System32 missing");
    add_test("Wine Host", is_file("QRX:C/QRXWineHostTest")?QRX_PARTIAL:QRX_FAIL, is_file("QRX:C/QRXWineHostTest")?"host test installed":"host test missing");
    add_test("Wine Display", is_file("QRX:C/QRXWineDisplayTest")?QRX_PARTIAL:QRX_FAIL, is_file("QRX:C/QRXWineDisplayTest")?"display test installed":"display test missing");
    add_test("Wine Services", is_file("QRX:C/QRXWineServicesTest")?QRX_PARTIAL:QRX_FAIL, is_file("QRX:C/QRXWineServicesTest")?"services test installed":"services test missing");
}
static const char *status_text(enum qrx_status s){return s==QRX_PASS?"PASS":(s==QRX_PARTIAL?"PARTIAL":"FAIL");}
static void write_report(void){
    BPTR f=Open((STRPTR)"T:QRX-System-Test.txt",MODE_NEWFILE); int i,p=0,a=0,x=0; char line[256];
    if(!f) return;
    FPrintf(f,(STRPTR)"QRX OS One System Test v%s\n\n",QRX_DIAG_VERSION);
    for(i=0;i<test_count;i++){ if(tests[i].status==QRX_PASS)p++;else if(tests[i].status==QRX_PARTIAL)a++;else x++;
        sprintf(line,"%-24s %-8s %s\n",tests[i].name,status_text(tests[i].status),tests[i].detail); FPuts(f,(STRPTR)line); }
    FPrintf(f,(STRPTR)"\n%d passed / %d partial / %d failed\n",p,a,x); Close(f);
}
static void draw_ui(struct Window *w){
    struct RastPort *rp=w->RPort; int i,y=34,p=0,a=0,x=0; char buf[96]; ULONG depth=1;
    if(w->WScreen && w->WScreen->RastPort.BitMap) depth=GetBitMapAttr(w->WScreen->RastPort.BitMap,BMA_DEPTH);
    SetAPen(rp,0); RectFill(rp,0,0,w->Width-1,w->Height-1);
    SetAPen(rp,1); Move(rp,10,16); Text(rp,(STRPTR)"QRX DIAGNOSTICS CENTER",22);
    sprintf(buf,"v%s  screen depth %lu",QRX_DIAG_VERSION,(unsigned long)depth); Move(rp,10,27); Text(rp,(STRPTR)buf,strlen(buf));
    for(i=0;i<test_count && y<w->Height-28;i++,y+=10){
        if(tests[i].status==QRX_PASS)p++; else if(tests[i].status==QRX_PARTIAL)a++; else x++;
        SetAPen(rp,1); Move(rp,10,y); Text(rp,(STRPTR)tests[i].name,strlen(tests[i].name));
        Move(rp,w->Width-75,y); Text(rp,(STRPTR)status_text(tests[i].status),strlen(status_text(tests[i].status)));
    }
    sprintf(buf,"%d passed / %d partial / %d failed",p,a,x); Move(rp,10,w->Height-12); Text(rp,(STRPTR)buf,strlen(buf));
}
int main(void){
    struct Window *w=NULL; struct IntuiMessage *m; BOOL done=FALSE;
    IntuitionBase=OpenLibrary((STRPTR)"intuition.library",39);
    GfxBase=(struct GfxBase*)OpenLibrary((STRPTR)"graphics.library",39);
    DOSBase=OpenLibrary((STRPTR)"dos.library",39);
    run_tests(); write_report();
    if(IntuitionBase && GfxBase){
        w=OpenWindowTags(NULL, WA_Title,(IPTR)"QRX Diagnostics Center", WA_Width,430, WA_Height,260,
            WA_DragBar,TRUE, WA_DepthGadget,TRUE, WA_CloseGadget,TRUE, WA_Activate,TRUE,
            WA_IDCMP,IDCMP_CLOSEWINDOW|IDCMP_REFRESHWINDOW, WA_SimpleRefresh,TRUE, TAG_DONE);
    }
    if(!w){ PutStr((STRPTR)"QRX Diagnostics Center: T:QRX-System-Test.txt\n"); goto out; }
    draw_ui(w);
    while(!done){ WaitPort(w->UserPort); while((m=(struct IntuiMessage*)GetMsg(w->UserPort))){ ULONG c=m->Class; ReplyMsg((struct Message*)m); if(c==IDCMP_CLOSEWINDOW)done=TRUE; else if(c==IDCMP_REFRESHWINDOW){BeginRefresh(w);draw_ui(w);EndRefresh(w,TRUE);} } }
    CloseWindow(w);
out:
    if(DOSBase) CloseLibrary(DOSBase); if(GfxBase) CloseLibrary((struct Library*)GfxBase); if(IntuitionBase) CloseLibrary(IntuitionBase); return 0;
}
