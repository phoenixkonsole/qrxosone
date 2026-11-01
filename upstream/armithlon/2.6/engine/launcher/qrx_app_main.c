#include "qrx_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc,char **argv){
    if(argc<3 || (strcmp(argv[1],"inspect")&&strcmp(argv[1],"launch"))){
        fprintf(stderr,"usage: qrx-app inspect PATH | qrx-app launch PATH\n");return 2;
    }
    QRXAppKind kind=qrx_app_inspect(argv[2]);
    if(!strcmp(argv[1],"inspect")){
        printf("%s: %s\n",argv[2],qrx_app_name(kind));
        return kind==QRX_APP_INVALID?1:0;
    }
    /* Explicit host selection for isolated AmigaOS-3.x QRX68 HLE.
       AROS native HUNK launch remains a separate path; NO guest process may
       silently spawn an executable on the host. */
    if(kind==QRX_APP_HUNK){
        const char *qrx68=getenv("QRX_HOST_68K");
        if(qrx68 && *qrx68){
            char *const args[]={(char*)qrx68,argv[2],NULL};
            execv(qrx68,args);
            perror("QRX68 compatibility runtime exec failed");return 127;
        }
    }
    /* Runtime paths must be selected explicitly. This is a host-side
       launcher, not permission for 68k guest applications to fork host apps. */
    const char *cmd[5];
    if(!qrx_app_plan(kind,getenv("QRX_HOST_WIN32"),getenv("QRX_HOST_LINUX"),argv[2],cmd,5)){
        fprintf(stderr,"QRX launch unsupported/unconfigured: %s\n",qrx_app_name(kind));return 3;
    }
    execv(cmd[0],(char *const *)cmd);
    perror("QRX runtime exec failed");return 127;
}
