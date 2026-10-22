#!/usr/bin/env python3
"""Prove physical sharing by walking both real CR3 page tables through QMP.

No guest interface exposes physical addresses. The observer runs on the host;
structure offsets are compiled from the actual paging source, not guessed.
"""
import argparse,hashlib,json,re,socket,struct,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
from lbs_kernel import install_kernel
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--memory',type=int,default=256);p.add_argument('--accel',choices=['kvm','tcg'],default='kvm');p.add_argument('--timeout',type=int,default=180);a=p.parse_args()
out=ROOT/f'tests/lbs/evidence/sharing/{a.accel}-{a.memory}m';out.mkdir(parents=True,exist_ok=True);(out/'result.json').unlink(missing_ok=True)
source=(ROOT/'kernel/mm/paging.c').read_text();end=source.index('} paging_process_space_t;')+len('} paging_process_space_t;');start=source.rfind('typedef struct {',0,end)
fields=source[start:end];tables=int(re.search(r'#define PAGING_PRIVATE_TABLE_MAX\s+(\d+)',source)[1]);slots=int(re.search(r'#define TASK_MAX\s+(\d+)',(ROOT/'kernel/include/task.h').read_text())[1])
layout=out/'layout.c';layout.write_text('#include <stdio.h>\n#include <stdint.h>\n#include <stdbool.h>\n#include <stddef.h>\n#define PAGING_PRIVATE_TABLE_MAX '+str(tables)+'\n'+fields+'\nint main(void){printf("%u %u %u",(unsigned)sizeof(paging_process_space_t),(unsigned)offsetof(paging_process_space_t,process_id),(unsigned)offsetof(paging_process_space_t,directory));}\n')
subprocess.run(['gcc','-m32',str(layout),'-o',str(out/'layout')],check=True);space_size,pid_off,pd_off=map(int,subprocess.check_output([str(out/'layout')],text=True).split())
nm=subprocess.check_output(['nm',str(ROOT/'build/kernel.elf')],text=True)
def symbol(name):return int(re.search(r'^([0-9a-fA-F]+) [bBdD] '+name+r'$',nm,re.M)[1],16)
spaces=symbol('g_process_spaces');cache_address=symbol('file_sources');gate_address=symbol('g_user_gate_page');heap_root=symbol('heap_start_address')
image=ROOT/f'build/lbs-sharing-{a.accel}-{a.memory}m.img';sock=ROOT/f'build/lbs-sharing-{a.accel}-{a.memory}m.sock';serial=out/'serial.log'
subprocess.run(['cp','--reflink=auto',str(ROOT/'build/bleskernos-ata-user.img'),str(image)],check=True)
install_kernel(image)
e=Editor(image);e.mkdir('/SYSTEM/TESTS/NATIVE')
inputs={'kernel_sha256':hashlib.sha256((ROOT/'build/kernel.bin').read_bytes()).hexdigest(),'stage2_sha256':hashlib.sha256((ROOT/'build/stage2.bin').read_bytes()).hexdigest()}
for lib in (ROOT/'build/system/libs/native').glob('*.LBS'):
 e.update('/SYSTEM/'+lib.name,lib.read_bytes());inputs[str(lib.relative_to(ROOT))]=hashlib.sha256(lib.read_bytes()).hexdigest()
for guest,host in [('LBSSHARE.BEX','build/tests/userland/LBSSHARE.BEX'),('FIXSHARE.LBS','build/tests/lbs/FIXSHARE.LBS')]:e.update('/SYSTEM/TESTS/NATIVE/'+guest,(ROOT/host).read_bytes());inputs[host]=hashlib.sha256((ROOT/host).read_bytes()).hexdigest()
e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',b'4|1|1|10|0|1|60000|Physical sharing|/SYSTEM/TESTS/NATIVE/LBSSHARE.BEX||Physical LBS sharing test\n');e.remove('/SYSTEM/USER/CONFIG/RECOVERY.NEXT');e.finish()
(out/'inputs.json').write_text(json.dumps(inputs,indent=2)+'\n');serial.unlink(missing_ok=True);sock.unlink(missing_ok=True)
cmd=['qemu-system-i386','-accel',a.accel,'-cpu','host' if a.accel=='kvm' else 'qemu32','-m',str(a.memory),'-smp','2','-vga','std','-drive',f'file={image},format=raw,if=ide','-display','none','-serial',f'file:{serial}','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot']
proc=subprocess.Popen(cmd,stderr=(out/'qemu.log').open('w'));client=None;stream=None;started=time.monotonic();baseline=None;before=None;after=None;released=False;report={}
def command(name,args=None):
 stream.write((json.dumps({'execute':name,'arguments':args or {}})+'\n').encode());stream.flush()
 while True:
  result=json.loads(stream.readline())
  if 'error' in result:raise RuntimeError(result['error'])
  if 'return' in result:return result['return']
def physical(address,size):
 target=out/'memory.bin';command('pmemsave',{'val':address,'size':size,'filename':str(target.resolve())});return target.read_bytes()
def directory(pid):
 table=physical(spaces,space_size*slots)
 for i in range(slots):
  if struct.unpack_from('<I',table,i*space_size+pid_off)[0]==pid:return struct.unpack_from('<I',table,i*space_size+pd_off)[0]
 return None
def ptes(pid,addresses):
 pd=directory(pid);assert pd,'No page directory for '+str(pid)
 page_directory=physical(pd,4096);pages={};result={}
 for address in addresses:
  pde=struct.unpack_from('<I',page_directory,(address>>22)*4)[0];assert pde&1 and pde&4 and not pde&128
  pt=pde&~4095
  if pt not in pages:pages[pt]=physical(pt,4096)
  pte=struct.unpack_from('<I',pages[pt],((address>>12)&1023)*4)[0]
  assert pte&1 and pte&4,(pid,hex(address),hex(pte))
  result[address]={'frame':pte&~4095,'writable':bool(pte&2),'cow_write':bool(pte&0x800)}
 return {'cr3':pd,'pages':result}
def ready(text):
 result={}
 for who,pid,code,const,data,value,maps,count in re.findall(r'\[LBSSHARE\] READY who=(\d+) pid=(\d+) code=([0-9a-f]+) constant=([0-9a-f]+) data=([0-9a-f]+) value=(\d+) maps=([0-9a-f]+) count=(\d+)\r?\n',text):
  result[int(who)]={'pid':int(pid),'code':int(code,16),'constant':int(const,16),'data':int(data,16),'value':int(value),'maps':int(maps,16),'count':int(count)}
 return result
def snapshot(text):
 children=ready(text);assert set(children)=={1,2};maps={1:{},2:{}}
 for who in [1,2]:
  child=children[who];assert 0<child['count']<=64
  address=child['maps'];length=child['count']*268
  pages=ptes(child['pid'],range(address&~4095,(address+length+4095)&~4095,4096))['pages']
  raw=bytearray();remaining=length;cursor=address
  while remaining:
   page=cursor&~4095;off=cursor&4095;chunk=min(remaining,4096-off)
   raw+=physical(pages[page]['frame']+off,chunk);cursor+=chunk;remaining-=chunk
  for index in range(child['count']):
   base,length=struct.unpack_from('<II',raw,index*268)
   name=raw[index*268+8:index*268+268].split(b'\0')[0].decode()
   assert name.endswith('.LBS') and not base&4095 and 0<length<16*1024*1024
   for off in range(0,length,4096):maps[who][(name,base+off)]=base+off
 # Compare module-relative pages: bases can differ between processes.
 ordered={}
 for who in [1,2]:
  entries={}
  for name in sorted({name for name,_ in maps[who]}):
   values=sorted(address for n,address in maps[who] if n==name)
   for index,address in enumerate(values):entries[(name,index)]=address
  ordered[who]=entries
 assert ordered[1].keys()==ordered[2].keys()
 translations={who:ptes(children[who]['pid'],list(ordered[who].values())+[children[who]['data'],gate_address]) for who in [1,2]}
 assert translations[1]['cr3']!=translations[2]['cr3']
 guard=struct.unpack('<I',physical(heap_root,4))[0]&~4095
 for who in [1,2]:
  gate=translations[who]['pages'][gate_address];assert not gate['writable']
  pd=physical(translations[who]['cr3'],4096);pde=struct.unpack_from('<I',pd,(guard>>22)*4)[0]
  table=physical(pde&~4095,4096);pte=struct.unpack_from('<I',table,((guard>>12)&1023)*4)[0]
  assert pte&1 and not pte&4,'allocator metadata guard weakened'
 assert translations[1]['pages'][gate_address]['frame']==translations[2]['pages'][gate_address]['frame']
 evidence=[]
 for key in ordered[1]:
  x=translations[1]['pages'][ordered[1][key]];y=translations[2]['pages'][ordered[2][key]]
  assert not x['writable'] and not y['writable'] and not x['cow_write'] and not y['cow_write']
  assert x['frame']==y['frame'],key
  evidence.append({'module':key[0],'page':key[1],'physical_frame':x['frame'],'virtual_one':ordered[1][key],'virtual_two':ordered[2][key]})
 x=translations[1]['pages'][children[1]['data']];y=translations[2]['pages'][children[2]['data']]
 assert x['frame']!=y['frame'] and x['writable'] and y['writable']
 return children,translations,evidence
try:
 while time.monotonic()-started<a.timeout:
  text=serial.read_text(errors='replace') if serial.exists() else ''
  if proc.poll() is not None:raise RuntimeError('QEMU exited')
  if '[LBSSHARE] FAIL' in text or 'KERNEL PANIC' in text:raise RuntimeError(text[-5000:])
  if stream is None and sock.exists():
   client=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);client.settimeout(15);client.connect(str(sock));stream=client.makefile('rwb');stream.readline();command('qmp_capabilities')
  if '[LBSSHARE] BASELINE\n' in text and baseline is None:
   command('stop');baseline=struct.unpack('<I',physical(cache_address,4))[0];command('cont')
  if '[LBSSHARE] BOTH READY\n' in text and before is None:
   command('stop');before=snapshot(text);command('cont');print('PASS physical RO sharing:',len(before[2]),'pages',flush=True)
  if '[LBSSHARE] COW READY\n' in text and after is None:
   assert before is not None;command('stop');children=before[0];after={who:ptes(children[who]['pid'],[children[who]['constant']]) for who in [1,2]}
   x=after[1]['pages'][children[1]['constant']];y=after[2]['pages'][children[2]['constant']]
   assert x['frame']!=y['frame'] and x['writable'] and not y['writable'];command('cont');print('PASS private COW detachment',flush=True)
  if '[LBSSHARE] RELEASED\n' in text and not released:
   command('stop');current=struct.unpack('<I',physical(cache_address,4))[0];assert current==baseline,(current,baseline)
   assert directory(before[0][1]['pid']) is None and directory(before[0][2]['pid']) is None
   command('cont');released=True;print('PASS last-reference cache and address-space release',flush=True)
  if '[LBSSHARE] ALL PASS\n' in text:
   assert before and after and released
   assert '[LBSSHARE] survivor data and code isolated' in text
   faults=re.findall(r'\[TASK\] excepcion Ring 3[^\n]*',text)
   assert len(faults)==1,(len(faults),faults)
   report={'status':'pass','memory_mib':a.memory,'accel':a.accel,'distinct_cr3':True,'readonly_pages_shared':len(before[2]),'physical_code_rodata_bytes_saved_with_two_processes':len(before[2])*4096,'private_data_frames_distinct':True,'cow_detaches_only_writer':True,'code_write_fault_observed':True,'last_reference_cache_released':True,'user_return_gates_readonly':True,'heap_metadata_supervisor_only':True,'elapsed_seconds':round(time.monotonic()-started,2),'page_evidence':before[2]};break
  time.sleep(.1)
 else:raise TimeoutError(text[-5000:])
finally:
 if stream:
  try:command('quit')
  except Exception:proc.terminate()
 else:proc.terminate()
 try:proc.wait(timeout=10)
 except subprocess.TimeoutExpired:proc.kill();proc.wait()
 if stream:stream.close()
 if client:client.close()
 sock.unlink(missing_ok=True);image.unlink(missing_ok=True);(out/'memory.bin').unlink(missing_ok=True)
if report:(out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:v for k,v in report.items() if k!='page_evidence'},indent=2))
