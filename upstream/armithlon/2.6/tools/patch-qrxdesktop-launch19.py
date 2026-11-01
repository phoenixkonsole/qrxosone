#!/usr/bin/env python3
"""Guarded patch of actual QRXDesktop 6.6.20 source from qrxosone.
Keeps existing desktop/window/file manager intact; only replaces synchronous
PE32/ELF launch via Execute() with nonblocking SystemTags() dispatch and
rejects filename characters that would break AmigaDOS quoted command arguments.
No patch is applied to the user's GitHub repository by this script.
"""
import argparse
import hashlib
from pathlib import Path

UPSTREAM_SHA256='fc9a3095abb0b8b8e3ecb7833d317da26f4287753672c7787ff83a83433cc23d'
TAG='/* QRXDesktop-1.9-Async-Safe-Guest-Dispatch */'
HEAD='''static LONG qrx_open_path(const char *full, LONG dir_type, UBYTE has_icon,
                          UBYTE icon_type, const char *default_tool) {'''


def patch(src: str) -> str:
    if TAG in src:
        return src
    if HEAD not in src or src.count(HEAD)!=1:
        raise ValueError('canonical QRXDesktop launch entry not found exactly once')
    before,tail=src.split(HEAD,1)
    cut=tail.find('\nstatic int qrx_copy_path(')
    if cut<0:
        raise ValueError('launch function end marker not found')
    body,after=tail[:cut],tail[cut:]
    snippets={
      '''        snprintf(command, sizeof(command), "QRX:C/QRXWinRun \\"%s\\"", full);
        execute_capture(command);
        return 1;''': 'QRX:C/QRXWinRun',
      '''            snprintf(command, sizeof(command), "QRX:C/QRXLinuxRun \\"%s\\"", full);
            execute_capture(command);
            return 1;''':'QRX:C/QRXLinuxRun',
      '''            snprintf(command, sizeof(command), "QRX:C/QRX86Run \\"%s\\"", full);
            execute_capture(command);
            return 1;''':'QRX:C/QRX86Run',
    }
    # Deliberately assert exact source snippets; upstream changes require review.
    for k,v in snippets.items():
        if body.count(k)!=1:
            raise ValueError(f'{v}: source has changed; expected unique dispatch snippet')
    for k,v in snippets.items():
        padding='        ' if v.endswith('QRXWinRun') else '            '
        body=body.replace(k,padding+f'return qrx19_launch_guest("{v}", full);')
    helper='''/* QRXDesktop-1.9-Async-Safe-Guest-Dispatch */
/* Existing QRX WinRun/LinuxRun/86Run commands are supplied by AROS QRX.
 * This function only dispatches safely; it does not emulate the binaries. */
static LONG qrx19_launch_guest(const char *runtime, const char *path) {
    char command[LEGACY_CMD_LEN];
    unsigned i;
    int written;
    if (!runtime || !path || !*path) return 0;
    for (i=0;path[i];i++) {
        unsigned char c=(unsigned char)path[i];
        if (c < 32 || c == 127 || c == '"' || c == '*' || c == ';' ||
            c == '$' || c == '`' || c == '\\x1b') {
            add_line("QRX86: unsafe filename; refusing to launch.");
            return 0;
        }
    }
    written=snprintf(command,sizeof(command),"%s \\"%s\\"",runtime,path);
    if (written<0 || (size_t)written>=sizeof(command)) {
        add_line("QRX86: guest command exceeds launcher capacity.");
        return 0;
    }
    return launch_native(command); /* asynchronous: keep desktop responsive */
}

'''
    return before+helper+HEAD+body+after


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('source',type=Path)
    ap.add_argument('output',type=Path)
    ap.add_argument('--allow-modified-upstream',action='store_true',help='for review of an already changed source')
    a=ap.parse_args()
    inp=a.source.read_bytes()
    sha=hashlib.sha256(inp).hexdigest()
    if TAG.encode() not in inp and sha!=UPSTREAM_SHA256 and not a.allow_modified_upstream:
        ap.error(f'Unexpected desktop source SHA256 {sha}; expected {UPSTREAM_SHA256}')
    data=patch(inp.decode('utf-8'))
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(data,encoding='utf-8')
    print(f'QRXDesktop 1.9 patched -> {a.output}, SHA256={hashlib.sha256(data.encode()).hexdigest()}')

if __name__=='__main__':main()
