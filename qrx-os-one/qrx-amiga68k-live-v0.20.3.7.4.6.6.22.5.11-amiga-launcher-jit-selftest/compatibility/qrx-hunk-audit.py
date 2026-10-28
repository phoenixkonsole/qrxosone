#!/usr/bin/env python3
"""Conservative read-only HUNK structure inspection; not an execution guarantee."""
import argparse, pathlib, struct, sys
HUNK_HEADER=0x3f3
HUNK_NAMES={0x3e7:"UNIT",0x3e8:"NAME",0x3e9:"CODE",0x3ea:"DATA",0x3eb:"BSS",0x3ec:"RELOC32",0x3f2:"END",0x3f3:"HEADER",0x3f7:"SYMBOL",0x3f8:"DEBUG"}
def inspect(path):
    data=pathlib.Path(path).read_bytes()
    if len(data)<4 or struct.unpack_from(">I",data)[0]!=HUNK_HEADER:
        raise ValueError("not a classic HUNK_HEADER executable (object files are not supported)")
    if len(data)%4:raise ValueError("truncated/non-word-aligned HUNK file")
    words=struct.unpack(">"+"I"*(len(data)//4),data); i=1
    def pop():
        nonlocal i
        if i>=len(words):raise ValueError("truncated HUNK")
        w=words[i];i+=1;return w
    while True:
        n=pop()
        if n==0:break
        if n>1024 or i+n>len(words):raise ValueError("invalid resident-library name")
        i+=n
    table=pop();first=pop();last=pop()
    if not 1<=table<=4096 or first>last or last>=table:raise ValueError("invalid hunk table")
    if i+table>len(words):raise ValueError("truncated hunk table")
    sizes=words[i:i+table];i+=table
    return {"hunks":table,"first":first,"last":last,"memory_words":sum(x&0x3fffffff for x in sizes),"bytes":len(data),"note":"header validated only; relocation/import/library compatibility NOT verified"}
if __name__=="__main__":
    ap=argparse.ArgumentParser();ap.add_argument("files",nargs="+");args=ap.parse_args();bad=False
    for f in args.files:
        try:print(f,inspect(f))
        except (OSError,ValueError,struct.error) as e:print(f,"ERROR",e,file=sys.stderr);bad=True
    sys.exit(1 if bad else 0)
