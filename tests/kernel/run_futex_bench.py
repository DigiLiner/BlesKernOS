#!/usr/bin/env python3
"""Run guest Linux regressions and measure futex handoffs or path lookups."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root/'tools'))
from bfs4_update import Editor

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--launch-delay',type=int,default=3,help='seconds after desktop startup; use 10 to avoid overlapping the idle report')
p.add_argument('--program',action='append',help='existing ports/wine/tests C test name; may repeat')
p.add_argument('--smp', type=int, default=2, choices=[1,2,4])
p.add_argument('--base', type=Path, default=root/'build/bleskernos-ata-user.img')
a = p.parse_args()
if not 0 <= a.launch_delay <= 600:p.error('--launch-delay must be between 0 and 600')
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=True)
image = out/'test.img'
if image.exists():
    p.error('use a new output directory')

def run(*args):
    subprocess.run([str(v) for v in args], cwd=root, check=True)

run('make', '-j2', 'build/stage2.bin', 'build/system/services/vision.o')
binary=out/'futex-suite.ELF'
flags=['gcc','-m32','-O2','-Wall','-Wextra','-no-pie','-pthread',
       '-Wl,--disable-new-dtags','-Wl,--dynamic-linker=/WINEGATE/lib/ld-linux.so.2','-Wl,-rpath,/WINEGATE/lib']
programs=a.program or ['futex-latency','futex','futex-mapping','futex-stress']
if any(not re.fullmatch(r'[a-z0-9-]+',name) for name in programs):p.error('invalid test name')
for name in programs:
    run(*flags,root/f'ports/wine/tests/{name}.c','-o',out/(name+'.ELF'))
source=out/'suite.c'
source.write_text('''#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
int main(void){
 setbuf(stdout,NULL);setenv("LD_LIBRARY_PATH","/WINEGATE/lib",1);
 const char *paths[]={TEST_PATHS};
 for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++){
  char path[128];snprintf(path,sizeof path,"/WINEGATE/bin/%s.ELF",paths[i]);
  pid_t child=fork();if(child<0)return 1;
  if(!child){execl(path,paths[i],NULL);_exit(127);}
  int status=0;if(waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status)){
   printf("[FUTEX-SUITE] FAIL test=%s status=%d\\n",paths[i],status);return 1;}
 }
 puts("[FUTEX-SUITE] PASS");return 0;
}
'''.replace('TEST_PATHS',','.join(json.dumps(name) for name in programs)))
run(*flags,source,'-o',binary)
run('cp', '--reflink=auto', a.base, image)
editor = Editor(image)
editor.update('/SYSTEM/SERVICES/VISION.BEX', (root/'build/system/services/vision.o').read_bytes())
editor.update('/WINEGATE/bin/futex-suite.ELF',binary.read_bytes())
if 'ipc' in programs:editor.update('/WINEGATE/bin/case01',binary.read_bytes())
for name in programs:
    editor.update('/WINEGATE/bin/'+name+'.ELF',(out/(name+'.ELF')).read_bytes())
editor.update('/SYSTEM/USER/CONFIG/SCREENSV.INI', b'enabled=0\ntimeout=3600\n')
editor.finish()
(out/'inputs.json').write_text(json.dumps({str(path.relative_to(root)):hashlib.sha256(path.read_bytes()).hexdigest() for path in [root/'kernel/linux/i386_futex.inc',root/'build/kernel.bin',*[root/f'ports/wine/tests/{name}.c' for name in programs]]},indent=2)+'\n')
run(sys.executable, 'ports/tinyx/scripts/build-stage2-kernel.py', image,
    '--launcher', '/WINEGATE/bin/futex-suite.ELF', '--launch-delay', str(a.launch_delay))
command = ['qemu-system-i386', '-accel', 'kvm', '-cpu', 'host', '-smp', str(a.smp), '-m', '1536',
           '-drive', f'file={image},format=raw,if=ide', '-vga', 'vmware', '-display', 'none',
           '-serial', f'file:{out}/serial.log', '-nodefaults', '-no-reboot', '-no-shutdown']
process = subprocess.Popen(command, stderr=(out/'qemu.log').open('w'))
begin = time.monotonic()
events = []
pending = ''
passed = False
finished = False
try:
    while not (out/'serial.log').exists():
        if process.poll() is not None:
            raise RuntimeError('QEMU exited before serial startup')
        time.sleep(.01)
    with (out/'serial.log').open(errors='replace') as serial:
        while time.monotonic()-begin < 300:
            pending += serial.read()
            lines = pending.split('\n')
            pending = lines.pop()
            for line in lines:
                events.append({'host_s': time.monotonic()-begin, 'line': line})
                if '[FUTEX' in line or '[IPC' in line:
                    print(events[-1], flush=True)
                if ('[FUTEX' in line and 'FAIL' in line) or '[PANIC]' in line:
                    raise RuntimeError(line)
                if '[GATE:STAGE2] RESULT' in line:
                    passed = 'status=0' in line
                    finished = True
                    break
            if finished:
                break
            if process.poll() is not None:
                raise RuntimeError('QEMU exited during benchmark')
            time.sleep(.01)
finally:
    process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill(); process.wait()
    (out/'host.json').write_text(json.dumps({'command':command, 'events':events}, indent=2)+'\n')
assert passed and any('[FUTEX-SUITE] PASS' in row['line'] for row in events), 'futex suite failed or timed out; see serial.log'
results=[]
for row in events:
    match=re.search(r'\[FUTEX-LATENCY\] run=(\d+) rounds=(\d+) us=(\d+) waits=(\d+),(\d+) wakes=(\d+),(\d+)',row['line'])
    if match:
        values=list(map(int,match.groups()))
        results.append(dict(zip(['run','rounds','us','waits_main','waits_peer','wakes_main','wakes_peer'],values)))
if 'futex-latency' in programs:assert len(results)==3, 'missing latency samples'
path_lookup=[]
for row in events:
    match=re.search(r'\[PATH-LOOKUP\] sample=(\d+) calls=(\d+) us=(\d+) size=(\d+)',row['line'])
    if match:path_lookup.append(dict(zip(['sample','calls','us','size'],map(int,match.groups()))))
ipc_latency=[]
for row in events:
    match=re.search(r'\[IPC-LATENCY\] poll=(\d+) sample=(\d+) rounds=(\d+) us=(\d+)',row['line'])
    if match:ipc_latency.append(dict(zip(['poll','sample','rounds','us'],map(int,match.groups()))))
if 'ipc-latency' in programs:assert len(ipc_latency)==6, 'missing IPC latency samples'
(out/'result.json').write_text(json.dumps({'result':'PASS','runs':results,'path_lookup':path_lookup,'ipc_latency':ipc_latency},indent=2)+'\n')
print(json.dumps(results,indent=2))
