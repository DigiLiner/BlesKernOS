#!/usr/bin/env python3
"""Attribute stacks in a paused, disposable Wine VM; no guest allocation."""
import argparse,json,re,socket,struct,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--name',default='stack-profile')
p.add_argument('--watermark',type=int,choices=[165],help='only for kernels built with the A5 stack watermark')
a=p.parse_args();out=a.out.resolve();v=json.loads((out/'vm.json').read_text())
source=out/'task-offsets.c'
source.write_text('''#include "kernel/include/task.h"
extern int printf(const char *,...);
#define O(m) ((unsigned)__builtin_offsetof(task_t,m))
int main(void){printf("%u %u %u %u %u %u %u %u %u %u %u %u %u",TASK_MAX,TASK_UNUSED,TASK_ZOMBIE,sizeof(task_t),O(pid),O(process_id),O(state),O(stack),O(stack_size),O(user_stack),O(user_stack_size),O(linux_abi),O(name));}
''')
subprocess.run(['gcc','-m32','-I'+str(ROOT),str(source),'-o',str(out/'task-offsets')],check=True)
keys=['count','unused','zombie','size','pid','process','state','stack','stack_size','user_stack','user_size','linux','name']
layout=dict(zip(keys,map(int,subprocess.check_output([str(out/'task-offsets')],text=True).split())))
symbols=subprocess.check_output(['nm',str(out/'kernel.elf')],text=True)
base=int(re.search(r'^([0-9a-f]+) b tasks$',symbols,re.M)[1],16)
s=socket.socket(socket.AF_UNIX);s.settimeout(30);s.connect(v['sock']);f=s.makefile('rwb',buffering=0)
f.readline()
def cmd(name,args=None):
    f.write((json.dumps({'execute':name,'arguments':args or {}})+'\n').encode())
    while True:
        r=json.loads(f.readline())
        if 'error' in r:raise RuntimeError(r)
        if 'return' in r:return r['return']
scratch=out/(a.name+'-scratch.bin')
def memory(address,size):
    assert 0<address<v['ram']*1024*1024 and size<=v['ram']*1024*1024-address
    cmd('pmemsave',{'val':address,'size':size,'filename':str(scratch)})
    data=scratch.read_bytes();scratch.unlink();return data
cmd('qmp_capabilities');cmd('stop')
try:
    data=memory(base,layout['size']*layout['count']);rows=[];groups={}
    for i in range(layout['count']):
        raw=data[i*layout['size']:(i+1)*layout['size']]
        def word(field):return struct.unpack_from('<I',raw,layout[field])[0]
        if word('state') in (layout['unused'],layout['zombie']):continue
        row={k:word(k) for k in ('pid','process','state','stack','stack_size','user_stack','user_size')}
        row['name']=raw[layout['name']:layout['name']+24].split(b'\0')[0].decode(errors='replace')
        row['linux']=bool(raw[layout['linux']]);rows.append(row)
        if a.watermark is not None and row['stack'] and row['stack_size']:
            body=memory(row['stack'],row['stack_size']);untouched=0
            while untouched<len(body) and body[untouched]==a.watermark:untouched+=1
            row['kernel_touched_max']=len(body)-untouched
        pid=str(row['process'] or row['pid'])
        g=groups.setdefault(pid,{'threads':0,'kernel_usable':0,'user_owned_usable':0,'kernel_touched_max':0})
        g['threads']+=1;g['kernel_usable']+=row['stack_size'];g['user_owned_usable']+=row['user_size']
        g['kernel_touched_max']=max(g['kernel_touched_max'],row.get('kernel_touched_max',0))
finally:
    cmd('cont');s.close();scratch.unlink(missing_ok=True)
result={'method':'paused physical read; usable stack sizes exclude allocator alignment/guard capacity; watermark is maximum touched depth, not a proof of every future call path','watermark':a.watermark,'layout':layout,'pids':groups,'tasks':rows}
(out/(a.name+'.json')).write_text(json.dumps(result,indent=2))
print(json.dumps(groups,indent=2))
