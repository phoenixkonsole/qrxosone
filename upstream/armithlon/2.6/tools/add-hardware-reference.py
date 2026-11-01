#!/usr/bin/env python3
"""Import verified QRX crossbench raw measurements for real 486/Pentium/68k/etc.
Measurements must be of the exact 300000-loop workload and checksum, not GHz,
Geekbench or a different Doom benchmark. Never fill missing CPU slots by guess.
"""
import argparse,json,re,statistics
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SLOTS=['486DX2','Pentium','68060','68080','Pentium 4','Core i7','Apple M1']
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--machine',choices=SLOTS,required=True)
parser.add_argument('--times',required=True,help='comma-separated positive seconds, ideally 3 independent runs')
parser.add_argument('--output-log',required=True,type=Path,help='raw original benchmark output containing valid checksum')
parser.add_argument('--source',required=True,help='physical hardware and measurement setup or link')
parser.add_argument('--out',type=Path,default=ROOT/'reports/reference/hardware-measurements.json')
a=parser.parse_args()
try:
    vals=[float(x) for x in a.times.split(',')]
    if not 1<=len(vals)<=20 or any(not 0<x<1000000 for x in vals):raise ValueError()
except ValueError:parser.error('Times must be 1..20 positive finite seconds')
raw=a.output_log.read_text()
if not re.search(r'QRXBENCH v=1 loops=300000 checksum=903be502',raw):
    parser.error('Raw log does not contain verified QRXBENCH checksum')
if len(a.source.strip())<8:parser.error('Describe CPU, clocks and how measured')
a.out.parent.mkdir(parents=True,exist_ok=True)
data={'schema':'qrx-matched-hardware-v1','machines':[]}
if a.out.exists():data=json.loads(a.out.read_text())
entry={'machine':a.machine,'workload':'qrx-crossbench-v1','iterations':300000,
       'checksum':'903be502','median_seconds':round(statistics.median(vals),6),
       'median_iterations_per_second':round(300000/statistics.median(vals),2),
       'raw_seconds':vals,'measurement_source':a.source,'record_type':'USER-SUPPLIED VERIFIED-CHECKSUM'}
data['machines']=[r for r in data['machines'] if r['machine']!=a.machine]+[entry]
a.out.write_text(json.dumps(data,indent=2,ensure_ascii=False)+'\n')
print(f'{a.machine}: {entry["median_iterations_per_second"]:.2f} matched-workload iterations/s saved to {a.out}')
