#!/usr/bin/env python3
"""Read allocated heap blocks in a paused test guest; no guest allocation."""
import argparse,json,re,socket,struct,tempfile
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);p.add_argument('--name',default='heap-profile')
a=p.parse_args();out=a.out.resolve();v=json.loads((out/'vm.json').read_text())
symbols={m[2]:int(m[1],16) for m in re.finditer(r'^([0-9a-f]+) [a-zA-Z] (\w+)$',subprocess.check_output(['nm',str(out/'kernel.elf')],text=True),re.M)}
s=socket.socket(socket.AF_UNIX);s.settimeout(30);s.connect(v['sock']);f=s.makefile('rwb',buffering=0);f.readline()
def cmd(name,args=None):
    f.write((json.dumps({'execute':name,'arguments':args or {}})+'\n').encode())
    while True:
        r=json.loads(f.readline())
        if 'error' in r:raise RuntimeError(r)
        if 'return' in r:return r['return']
with tempfile.TemporaryDirectory(prefix='bk-heap-profile-',dir='/dev/shm') as folder:
    scratch=Path(folder)/'memory.bin'
    def memory(address,size):
        assert 0<address<v['ram']*1048576 and 0<size<=v['ram']*1048576-address
        cmd('pmemsave',{'val':address,'size':size,'filename':str(scratch)});return scratch.read_bytes()
    cmd('qmp_capabilities');cmd('stop')
    try:
        def symbol_word(name):return struct.unpack('<I',memory(symbols[name],4))[0]
        start=symbol_word('heap_start_address');end=symbol_word('heap_end_address');head=symbol_word('heap_head');reserved=symbol_word('heap_exec_reservation')
        data=memory(start,end-start)
    finally:cmd('cont');s.close()
rows=[];groups={};seen=set();pointer=head
while pointer:
    assert start<=pointer<=end-24 and pointer not in seen;seen.add(pointer)
    magic,size,free,subsystem,owner,next,prev=struct.unpack_from('<IIBB2xIII',data,pointer-start)
    assert magic==0xb1e5c0de and pointer+24+size<=end
    if not free and pointer!=reserved:
        row={'address':pointer+24,'bytes':size,'owner':owner,'subsystem':subsystem};rows.append(row)
        key=str(owner)+':'+str(subsystem);g=groups.setdefault(key,{'bytes':0,'blocks':0});g['bytes']+=size;g['blocks']+=1
    pointer=next
result={'method':'paused heap payload blocks; excludes free blocks and ET_EXEC reservation; payload bytes are not deduplicated physical-page statistics','blocks':len(rows),'groups':groups,'largest':sorted(rows,key=lambda r:-r['bytes'])[:200]}
(out/(a.name+'.json')).write_text(json.dumps(result,indent=2));print(json.dumps({'blocks':len(rows),'largest':result['largest'][:12]}))
