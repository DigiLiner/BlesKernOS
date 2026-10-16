#!/usr/bin/env python3
"""Measure GLX frame counters in an active game; these are not scanout FPS."""
import argparse,json,re,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('out',type=Path);p.add_argument('--seconds',type=int,default=20);p.add_argument('--samples',type=int,default=3);p.add_argument('--name',default='frame-measurement');a=p.parse_args()
if a.seconds<1 or a.samples<1:p.error('seconds and samples must be positive')
rejected_serial_rows=0
def latest():
 global rejected_serial_rows
 text=(a.out/'serial.log').read_text(errors='replace')
 rows=re.findall(r'^\[WINE:FRAME-PERF\] frames=(\d+) drawable=(\d+).*?elapsed_us=(\d+) resolve_us=(\d+) present_us=(\d+) copy_us=(\d+)\r?\n',text,re.M)
 if not rows:raise RuntimeError('no frame counters')
 valid={};last=None;rejected_serial_rows=0
 for fields in rows:
  row=list(map(int,fields));previous=valid.get(row[1])
  # COM1 can interleave/truncate diagnostic text. A cumulative timing
  # counter cannot decrease within the same advancing context sequence.
  if previous and row[0]>=previous[0] and any(row[i]<previous[i] for i in [2,3,4,5]):
   rejected_serial_rows+=1;continue
  valid[row[1]]=row;last=row
 if last is None:raise RuntimeError('no valid frame counters')
 return last
result=[]
for sample in range(a.samples):
 start=latest();host=time.monotonic();deadline=host+a.seconds
 while time.monotonic()<deadline:time.sleep(min(.25,deadline-time.monotonic()) if deadline>time.monotonic() else .001)
 end=latest();duration=time.monotonic()-host
 if start[1]!=end[1] or end[0]<=start[0] or end[2]<=start[2]:raise RuntimeError('drawable changed or stopped advancing')
 row={'sample':sample,'rejected_serial_rows':rejected_serial_rows,'host_seconds':duration,'start':start,'end':end,'presents':end[0]-start[0],'host_presents_per_second':(end[0]-start[0])/duration,'guest_seconds':(end[2]-start[2])/1e6,'presents_per_second':(end[0]-start[0])*1e6/(end[2]-start[2]),'resolve_seconds':(end[3]-start[3])/1e6,'present_seconds':(end[4]-start[4])/1e6,'copy_seconds':(end[5]-start[5])/1e6}
 if any(v<0 for v in [row['resolve_seconds'],row['present_seconds'],row['copy_seconds']]) or row['resolve_seconds']+row['present_seconds']+row['copy_seconds']>row['guest_seconds']*2+1:raise RuntimeError('invalid timing counters in serial sample')
 result.append(row);print(json.dumps(row),flush=True)
(a.out/(a.name+'.json')).write_text(json.dumps(result,indent=2)+'\n')
