#!/usr/bin/env python3
"""Hermetic test of benchmark runner classification; no M1/Win/Linux claims."""
from pathlib import Path
import tempfile,stat,json,importlib.util
here=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('cross',here/'bench-qrx86-execution.py')
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
with tempfile.TemporaryDirectory() as d:
    d=Path(d)
    ok=d/'ok'; ok.write_text('#!/bin/sh\necho "QRXBENCH v=1 loops=300000 checksum=903be502"\necho "qrx-i386: pid 0 executed 25000000 guest instructions" >&2\n')
    ok.chmod(0o755)
    res=mod.run('guest',[ok],2,10)
    assert res['status']=='PASS' and len(res['runs'])==2
    assert res['median_guest_instructions_per_second']>0
    broken=d/'broken'; broken.write_text('#!/bin/sh\necho "QRXBENCH v=1 loops=300000 checksum=ffffffff"\n');broken.chmod(0o755)
    assert mod.run('invalid',[broken],1,10)['status']=='FAIL'
    absent=d/'absent';assert mod.run('missing',[absent],1,10)['status']=='SKIP'
    report={'timestamp_utc':'test','platform':'test','results':[res,{'label':'unknown','status':'SKIP'}],
            'hardware_reference':[]}
    mod.render_html(report,d/'dashboard.html')
    html=(d/'dashboard.html').read_text()
    assert '486DX2' in html and 'Noch keine validierte Vergleichsmessung' in html
print('PASS QRX86 execution benchmark: pass/fail/skip/stats, honest reference dashboard')
