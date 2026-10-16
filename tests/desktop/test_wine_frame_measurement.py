#!/usr/bin/env python3
"""A concurrently appended serial line must not become a partial counter."""
from pathlib import Path
import re,tempfile,types
root=Path(__file__).resolve().parents[2]
s=(root/'tools/measure_wine_frames.py').read_text()
with tempfile.TemporaryDirectory(prefix='bk-frame-reader-') as directory:
 p=Path(directory)/'serial.log'
 def row(frame,elapsed,copy):
  return f'[WINE:FRAME-PERF] frames={frame} drawable=7 size=640x480 elapsed_us={elapsed} resolve_us=1 present_us=1 copy_us={copy}'
 ns={'a':types.SimpleNamespace(out=Path(directory)),'re':re}
 exec(s[s.index('def latest():'):s.index('result=[]')],ns)
 p.write_text(row(100,1000000,900)+'\n'+row(101,2000000,1000))
 assert ns['latest']()[0]==100  # missing newline: still being written
 with p.open('a') as f:f.write('\n')
 assert ns['latest']()==[101,7,2000000,1,1,1000]
 with p.open('a') as f:f.write(row(102,3000000,100)+'\n')
 assert ns['latest']()[0]==101 and ns['rejected_serial_rows']==1
 with p.open('a') as f:f.write(row(1,100,0)+'\n')
 assert ns['latest']()[0]==1  # a newly created context can restart counters
 print('PASS incomplete serial snapshots, completed lines, corrupt cumulative counters and context reset')
