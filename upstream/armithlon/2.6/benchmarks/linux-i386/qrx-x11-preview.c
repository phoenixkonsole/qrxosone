/* QRX-OS-One / ARMITHLON 1.2, original minimal static Linux ELF32/i386 X11 demo.
 * SPDX-License-Identifier: MIT
 * Does not require libc, Xlib, glibc or a Linux kernel. It uses QRX86's
 * socketcall emulation and internal X11 :0 server. Color bars plus one rectangle.
 * Can be executed as an ELF32/i386 binary on M1 through qrx-i386.
 */
typedef unsigned int u32;
static int syscall3(u32 num,u32 a,u32 b,u32 c){
 int result;
 __asm__ volatile("int $0x80":"=a"(result):"a"(num),"b"(a),"c"(b),"d"(c):"memory","cc");
 return result;
}
static void exit_now(int code){syscall3(1,(u32)code,0,0);for(;;){}}
static void check(int good,int error){if(!good)exit_now(error);}
static void wr16(unsigned char *p,u32 v){p[0]=v;p[1]=v>>8;}
static void wr32(unsigned char *p,u32 v){wr16(p,v);wr16(p+2,v>>16);}
static unsigned char setup[128], requests[256];
static const unsigned char sockaddr_x11[] = {1,0,'/','t','m','p','/','.','X','1','1','-','u','n','i','x','/','X','0',0};
static const unsigned char hello[12]={'l',0,11,0,0,0,0,0,0,0,0,0};
static void run(void){
 u32 args[4]={1,1,0,0};
 int fd=syscall3(102,1,(u32)args,0);check(fd>=3,10);
 args[0]=fd;args[1]=(u32)sockaddr_x11;args[2]=sizeof(sockaddr_x11);
 check(syscall3(102,3,(u32)args,0)==0,11);
 check(syscall3(4,fd,(u32)hello,sizeof(hello))==12,12);
 check(syscall3(3,fd,(u32)setup,8)==8 && setup[0]==1,13);
 check(syscall3(3,fd,(u32)setup,120)==120,14);
 unsigned char *p=requests;
 /* X11 CreateWindow: depth 24, parent root, 256x192, white background. */
 p[0]=1;p[1]=24;wr16(p+2,10);wr32(p+4,0x200001);wr32(p+8,1);
 wr16(p+16,256);wr16(p+18,192);wr16(p+22,1);wr32(p+24,3);
 wr32(p+28,(1u<<1)|(1u<<11));wr32(p+32,0x00101124);wr32(p+36,1u<<15);p+=40;
 /* MapWindow. */
 p[0]=8;wr16(p+2,2);wr32(p+4,0x200001);p+=8;
 /* CreateGC foreground electric blue. */
 p[0]=55;wr16(p+2,5);wr32(p+4,0x200002);wr32(p+8,0x200001);
 wr32(p+12,4);wr32(p+16,0x002299ff);p+=20;
 /* PolyFillRectangle first. */
 p[0]=70;wr16(p+2,5);wr32(p+4,0x200001);wr32(p+8,0x200002);
 wr16(p+12,16);wr16(p+14,16);wr16(p+16,220);wr16(p+18,50);p+=20;
 /* ChangeGC foreground green. */
 p[0]=56;wr16(p+2,4);wr32(p+4,0x200002);wr32(p+8,4);wr32(p+12,0x0036eaa3);p+=16;
 /* PolyFillRectangle second. */
 p[0]=70;wr16(p+2,5);wr32(p+4,0x200001);wr32(p+8,0x200002);
 wr16(p+12,16);wr16(p+14,82);wr16(p+16,220);wr16(p+18,50);p+=20;
 int bytes=(int)(p-requests);
 check(syscall3(4,(u32)fd,(u32)requests,(u32)bytes)==bytes,15);
 check(syscall3(6,(u32)fd,0,0)==0,16);
 static const char result[]="QRX X11 preview: window and GPU frame dispatched\n";
 syscall3(4,1,(u32)result,sizeof(result)-1);
 exit_now(0);
}
void _start(void){run();}
