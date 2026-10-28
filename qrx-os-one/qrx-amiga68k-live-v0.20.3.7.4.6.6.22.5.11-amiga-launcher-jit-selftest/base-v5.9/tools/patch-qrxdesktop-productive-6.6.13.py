#!/usr/bin/env python3
from pathlib import Path
import sys
p=Path(sys.argv[1]);s=p.read_text(); mark='QRX 6.6.13 productive desktop audit'
if mark in s: raise SystemExit(0)
s='/* '+mark+' */\n'+s
# terminal history state
needle='static int input_len = 0;'
assert needle in s
s=s.replace(needle,needle+'\n#define QRX_HIST 16\nstatic char qrx_hist[QRX_HIST][INPUT_LEN+1]; static int qrx_hist_count=0,qrx_hist_pos=0; static int qrx_term_scroll=0;\nstatic void qrx_hist_add(const char *v){int i;if(!v||!*v)return;if(qrx_hist_count&&strcmp(qrx_hist[qrx_hist_count-1],v)==0)return;if(qrx_hist_count>=QRX_HIST){for(i=1;i<QRX_HIST;i++)strcpy(qrx_hist[i-1],qrx_hist[i]);qrx_hist_count=QRX_HIST-1;}strncpy(qrx_hist[qrx_hist_count++],v,INPUT_LEN);qrx_hist[qrx_hist_count-1][INPUT_LEN]=0;qrx_hist_pos=qrx_hist_count;}\nstatic void qrx_hist_get(int d){if(!qrx_hist_count)return;qrx_hist_pos+=d;if(qrx_hist_pos<0)qrx_hist_pos=0;if(qrx_hist_pos>qrx_hist_count)qrx_hist_pos=qrx_hist_count;if(qrx_hist_pos==qrx_hist_count){input[0]=0;input_len=0;}else{strcpy(input,qrx_hist[qrx_hist_pos]);input_len=strlen(input);}}\nstatic void qrx_complete(void){static const char*cmds[]={"help","version","info","mem","clear","about","writer","sheet","presenter","hardware","files","htop","ai","compat","qrx86","x86run","winrun","wineboot","reactosnotepad",0};int i,n=0,hit=-1;for(i=0;cmds[i];i++)if(!strncmp(cmds[i],input,input_len)){n++;hit=i;}if(n==1){strncpy(input,cmds[hit],INPUT_LEN);input[INPUT_LEN]=0;input_len=strlen(input);}}')
# Close terminal => back, not session exit; front on decoration
s=s.replace('if (terminal_close_hit(mx, my)) return 0;','if (terminal_close_hit(mx, my)) { WindowToBack(g_terminal); return 1; }')
s=s.replace('if (terminal_resize_hit(mx, my))\n                begin_window_gesture(mx, my, 1);','WindowToFront(g_terminal); ActivateWindow(g_terminal);\n            if (terminal_resize_hit(mx, my))\n                begin_window_gesture(mx, my, 1);')
# history add before command execution
s=s.replace('input[input_len] = 0;\n            if (!execute_command(input)) return 0;','input[input_len] = 0;\n            qrx_hist_add(input);\n            if (!execute_command(input)) return 0;')
# RAWKEY history/tab/scroll
raw=''' else if (cls == IDCMP_RAWKEY) {
        if (code == 0x4c) { qrx_hist_get(-1); redraw_terminal(); }
        else if (code == 0x4d) { qrx_hist_get(1); redraw_terminal(); }
        else if (code == 0x42) { qrx_complete(); redraw_terminal(); }
        else if (code == 0x4f) { if(qrx_term_scroll<line_count-1)qrx_term_scroll++; redraw_terminal(); }
        else if (code == 0x4e) { if(qrx_term_scroll>0)qrx_term_scroll--; redraw_terminal(); }
    }'''
idx=s.find('\n    return 1;\n}',s.find('static int handle_terminal_message'))
assert idx>0
s=s[:idx]+raw+s[idx:]
s=s.replace('IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_NEWSIZE,','IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_NEWSIZE | IDCMP_RAWKEY,',1)
# Terminal visual wrapping + scrolling: replace redraw body
start=s.find('static void redraw_terminal(void) {'); end=s.find('\nstatic ',start+20)
assert start>0 and end>start
fn=r'''static void redraw_terminal(void) {
    struct RastPort *rp; int i,y,cols,startline,used=0; char prompt[INPUT_LEN+12],subtitle[96],part[96]; const char *q; int n;
    if(!g_terminal)return;rp=g_terminal->RPort;draw_terminal_chrome(rp,g_terminal->Width,g_terminal->Height);
    snprintf(subtitle,sizeof(subtitle),"native 68K / %s / QRX   Up/Down history  TAB complete",look_name(g_look));text_at(rp,PEN_MUTED,14,48,subtitle);
    cols=(g_terminal->Width-32)/8;if(cols<20)cols=20;if(cols>90)cols=90;startline=qrx_term_scroll;if(startline<0)startline=0;y=65;
    for(i=startline;i<line_count&&y<g_terminal->Height-30;i++){q=lines[i];if(!*q){y+=11;continue;}while(*q&&y<g_terminal->Height-30){n=strlen(q);if(n>cols)n=cols;if(q[n]&&n==cols){int c=n;while(c>8&&q[c]!=' ')c--;if(c>8)n=c;}memcpy(part,q,n);part[n]=0;text_at(rp,PEN_TEXT,14,y,part);q+=n;while(*q==' ')q++;y+=11;used++;}}
    snprintf(prompt,sizeof(prompt),"qrx:> %s_",input); if((int)strlen(prompt)>cols) text_at(rp,PEN_ACCENT,14,g_terminal->Height-15,prompt+strlen(prompt)-cols); else text_at(rp,PEN_ACCENT,14,g_terminal->Height-15,prompt);
}'''
s=s[:start]+fn+s[end:]
p.write_text(s)
print('6.6.13 productive desktop patch OK')
