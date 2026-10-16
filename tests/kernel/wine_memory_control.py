#!/usr/bin/env python3
"""One short QMP interaction with the disposable Wine memory gate."""
import argparse,json,socket,struct,time
from pathlib import Path
from PIL import Image
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True)
p.add_argument('--shot',default='current');p.add_argument('--keys',nargs='*',default=[])
p.add_argument('--click',nargs=2,type=int);p.add_argument('--focus');p.add_argument('--quit',action='store_true')
p.add_argument('--text',help='type ASCII text after --keys in the disposable VM')
p.add_argument('--registers',action='store_true')
p.add_argument('--overlay',type=Path,help='put future writes of this test VM in a disposable qcow2 overlay')
a=p.parse_args();out=a.out.resolve();v=json.loads((out/'vm.json').read_text());layout=v['layout']
s=socket.socket(socket.AF_UNIX);s.settimeout(20);s.connect(v['sock']);f=s.makefile('rwb',buffering=0);f.readline()
def cmd(name,args=None):
 f.write((json.dumps({'execute':name,'arguments':args or {}})+'\n').encode())
 while True:
  response=json.loads(f.readline())
  if 'error' in response:raise RuntimeError(response)
  if 'return' in response:return response['return']
cmd('qmp_capabilities')
def memory(address,size):
 file=out/'qmp-memory.bin';cmd('pmemsave',{'val':address,'size':size,'filename':str(file)});result=file.read_bytes();file.unlink();return result
def windows():
 cmd('stop')
 try:
  desktop=struct.unpack('<I',memory(v['desktop'],4))[0];data=memory(desktop,layout['mouse_y']+4)
  pointer=struct.unpack_from('<I',data,layout['first'])[0];result=[];seen=set()
  while pointer and pointer not in seen and len(seen)<128:
   assert 0x400000<=pointer<v['ram']*1024*1024
   seen.add(pointer);w=memory(pointer,layout['size'])
   result.append({'handle':pointer,'title':w[layout['title']:layout['title']+48].split(b'\0')[0].decode(errors='replace'),'bounds':struct.unpack_from('<iiii',w,layout['bounds']),'owner':struct.unpack_from('<I',w,layout['owner'])[0],'visible':bool(w[layout['visible']]),'listed':bool(w[layout['listed']])})
   if 'input_enabled' in layout:result[-1]['input_enabled']=bool(w[layout['input_enabled']])
   pointer=struct.unpack_from('<I',w,layout['next'])[0]
  return result,(struct.unpack_from('<i',data,layout['mouse_x'])[0],struct.unpack_from('<i',data,layout['mouse_y'])[0])
 finally:cmd('cont')
def click(x,y):
 for attempt in range(100):
  _,position=windows();dx=max(-40,min(40,x-position[0]));dy=max(-40,min(40,y-position[1]))
  if abs(x-position[0])<=2 and abs(y-position[1])<=2:break
  cmd('human-monitor-command',{'command-line':f'mouse_move {dx} {dy}'});time.sleep(.3)
 else:raise TimeoutError('mouse movement')
 cmd('human-monitor-command',{'command-line':'mouse_button 1'});time.sleep(.15)
 cmd('human-monitor-command',{'command-line':'mouse_button 0'});time.sleep(.8)
try:
 if a.overlay:
  blocks=cmd('query-block');device=next(b['device'] for b in blocks if b.get('inserted',{}).get('file')==v.get('image',str(out/'test.img')))
  cmd('blockdev-snapshot-sync',{'device':device,'snapshot-file':str(a.overlay.resolve()),'format':'qcow2'})
  (out/'overlay.json').write_text(json.dumps({'path':str(a.overlay.resolve()),'device':device},indent=2))
 if a.registers:
  cmd('stop')
  try:
   for cpu in range(v.get('smp',2)):
    registers=cmd('human-monitor-command',{'command-line':'info registers','cpu-index':cpu})
    (out/(a.shot+'-cpu'+str(cpu)+'.txt')).write_text(registers)
    import re
    esp=int(re.search(r'ESP=([0-9a-fA-F]+)',registers)[1],16)
    if esp<v['ram']*1024*1024:
     (out/(a.shot+'-stack'+str(cpu)+'.bin')).write_bytes(memory(esp,2048))
  finally:cmd('cont')
 items,_=windows();(out/'windows.json').write_text(json.dumps(items,indent=2)+'\n')
 if a.focus:
  target=next(w for w in reversed(items) if a.focus.lower() in w['title'].lower() and w['visible'] and w['listed'])
  x,y,w,h=target['bounds']
  # Pick an exposed title-bar point. The small monitor may be partly covered
  # by a Wine dialog; clicking its covered left edge focuses that dialog.
  index=items.index(target);above=[v for v in items[index+1:] if v['visible'] and v['listed']]
  candidates=[(x+30,y+12),(x+w-60,y+12),(x+w//2,y+12)]
  point=next(((px,py) for px,py in candidates if not any(
      v['bounds'][0]<=px<v['bounds'][0]+v['bounds'][2] and
      v['bounds'][1]<=py<v['bounds'][1]+v['bounds'][3] for v in above)),None)
  if point is None:raise RuntimeError('Target title bar is fully covered')
  click(*point)
 if a.click:click(*a.click)
 typed=[]
 for char in a.text or '':
  if char.isascii() and char.isalpha():typed.append(('shift-' if char.isupper() else '')+char.lower())
  elif char in '0123456789':typed.append(char)
  else:typed.append({' ': 'spc','.':'dot',':':'shift-semicolon','\\':'backslash','/':'slash','-':'minus','_':'shift-minus'}[char])
 for key in a.keys+typed:
  codes=key.split('-')
  for down,ordered in [(True,codes),(False,list(reversed(codes)))]:
   cmd('input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':code}}} for code in ordered]});time.sleep(.1)
  time.sleep(.5)
 file=out/(a.shot+'.ppm');cmd('screendump',{'filename':str(file)});Image.open(file).save(file.with_suffix('.png'))
 print(json.dumps(windows()[0],indent=2))
 if a.quit:cmd('quit')
finally:s.close()
