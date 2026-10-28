#include <stdlib.h>
#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <proto/bsdsocket.h>
#include <string.h>
#include <stdio.h>
#include "qrx_net.h"
#if defined(__AROS__)
extern int snprintf(char *, size_t, const char *, ...);
extern int atoi(const char *);
#endif
struct Library *SocketBase=0;
static int open_net(void){if(SocketBase)return 1;SocketBase=OpenLibrary((CONST_STRPTR)"bsdsocket.library",4);return SocketBase!=0;}
int qrx_net_available(void){int ok=open_net();return ok;}
static int parse_http(const char*u,char*host,int hc,char*path,int pc,int*port){const char*p=u;const char*s;int n;if(strncmp(p,"http://",7))return 0;p+=7;s=strchr(p,'/');n=s?(int)(s-p):(int)strlen(p);if(n<=0||n>=hc)return 0;memcpy(host,p,n);host[n]=0;strncpy(path,s?s:"/",pc-1);path[pc-1]=0;*port=80;{char*c=strchr(host,':');if(c){*c=0;*port=atoi(c+1);}}return 1;}
int qrx_net_http_get(const char*url,char*out,int cap,char*status,int sc){char host[256],path[512],req[1024];int port,fd=-1,n,total=0,body=0;struct hostent*he;struct sockaddr_in sa;if(!out||cap<2)return 0;out[0]=0;if(!open_net()){snprintf(status,sc,"bsdsocket.library unavailable");return 0;}if(!parse_http(url,host,sizeof(host),path,sizeof(path),&port)){snprintf(status,sc,"not an http:// URL");return 0;}he=gethostbyname(host);if(!he||!he->h_addr_list||!he->h_addr_list[0]){snprintf(status,sc,"DNS failed: %s",host);return 0;}memset(&sa,0,sizeof(sa));sa.sin_family=AF_INET;sa.sin_port=htons((UWORD)port);memcpy(&sa.sin_addr,he->h_addr_list[0],sizeof(sa.sin_addr));fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0){snprintf(status,sc,"socket() failed");return 0;}if(connect(fd,(struct sockaddr*)&sa,sizeof(sa))<0){CloseSocket(fd);snprintf(status,sc,"connect failed: %s:%d",host,port);return 0;}snprintf(req,sizeof(req),"GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: QRXWeb/68K\r\nAccept: text/html,text/plain,*/*\r\nConnection: close\r\n\r\n",path,host);if(send(fd,req,strlen(req),0)<0){CloseSocket(fd);snprintf(status,sc,"send failed");return 0;}while(total<cap-1&&(n=recv(fd,out+total,cap-1-total,0))>0)total+=n;CloseSocket(fd);out[total]=0;{char*p=strstr(out,"\r\n\r\n");if(p){body=(int)(p+4-out);memmove(out,out+body,total-body+1);total-=body;}else{p=strstr(out,"\n\n");if(p){body=(int)(p+2-out);memmove(out,out+body,total-body+1);total-=body;}}}snprintf(status,sc,"native TCP/DNS HTTP: %s:%d (%d bytes)",host,port,total);return 1;}
