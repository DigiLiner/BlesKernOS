import subprocess,time,re,json,socket,sys
from pathlib import Path
root=Path('/tmp/blesk-round2')
results=json.loads((root/"bench-result.json").read_text()) if (root/"bench-result.json").exists() else {}
for name in sys.argv[1:] or ['baseline-ahci','final-ahci','baseline-ide','final-ide','tests']:
 d=root/name; log=d/'serial.log';monitor=d/'monitor.sock'
 ahci='ahci' in name
 drive_file=f'{d}/test.img'
 if (d/'error.conf').exists():drive_file=f'blkdebug:{d}/error.conf:{drive_file}'
 disk=['-drive',f'file={drive_file},format=raw,if=none,id=osdisk','-device','ich9-ahci,id=ahci','-device','ide-hd,drive=osdisk,bus=ahci.0'] if ahci else ['-drive',f'file={drive_file},format=raw,if=ide']
 cmd=['qemu-system-i386','-accel','kvm','-cpu','pentium3','-smp','2','-m','1536',*disk,'-vga','vmware','-display','none','-serial',f'file:{log}','-monitor',f'unix:{monitor},server=on,wait=off','-no-reboot','-no-shutdown','-drive','file=media/installers/OFF97.iso,media=cdrom,readonly=on']
 print('START',name,flush=True);p=subprocess.Popen(cmd);start=time.monotonic()
 try:
  while time.monotonic()-start<240:
   time.sleep(1);s=log.read_text(errors='replace') if log.exists() else ''
   ready=re.search(r'\[WINE:BOOT\] READY elapsed_ms=(\d+)',s)
   app=re.search(r'\[WINE:PERF\] ready total_ms=(\d+)',s)
   if name.endswith('tests'):
    done=bool(re.search(r'\[GATE:STAGE2\] RESULT \S+ status=0',s))
    if re.search(r'\[GATE:STAGE2\] RESULT \S+ status=(?!0)\S+',s):raise RuntimeError(f'{name} failed')
   else:done=bool(ready and (name.startswith('baseline') or app))
   if done:
    results[name]={'wine_ms':int(ready[1]) if ready else None,'app_ms':int(app[1]) if app else None,'wall_s':round(time.monotonic()-start,1),'cmd':cmd}
    print('DONE',name,results[name],flush=True)
    if app:
     time.sleep(5)
     sock=socket.socket(socket.AF_UNIX);sock.connect(str(monitor));sock.recv(4096)
     if name.startswith('final2'):
      for key in ['b','l','e','s']:
       sock.sendall(f'sendkey {key}\n'.encode());time.sleep(.2)
      time.sleep(1)
     sock.sendall(f'screendump {d}/notepad.ppm\n'.encode());time.sleep(.4);sock.close()
    break
   if p.poll() is not None:raise RuntimeError(f'{name} exited {p.returncode}')
  else:raise RuntimeError(f'{name} timed out')
 finally:
  p.terminate()
  try:p.wait(timeout=5)
  except subprocess.TimeoutExpired:p.kill();p.wait()
 (root/'bench-result.json').write_text(json.dumps(results,indent=2)+'\n')
