#!/usr/bin/env python3
"""Install kernel/video/process monitor in an offline image, preserving user data."""
import argparse,os,re,shutil,subprocess,sys
from pathlib import Path
import bfs4
from bfs4_update import Editor
ROOT=Path(__file__).resolve().parents[1]
def set_master_volume(data, volume):
 if not 0 <= volume <= 100:raise ValueError('master volume must be 0..100')
 value=b'MasterVolume='+str(volume).encode('ascii')
 if re.search(rb'^MasterVolume=[^\r\n]*',data,re.M):
  return re.sub(rb'^MasterVolume=[^\r\n]*',lambda match:value,data,flags=re.M)
 return data+(b'' if not data or data.endswith(b'\n') else b'\r\n')+value+b'\r\n'
def offline(image):
 for proc in Path('/proc').iterdir():
  if not proc.name.isdecimal():continue
  try:
   if 'qemu-system' not in (proc/'comm').read_text():continue
   for fd in (proc/'fd').iterdir():
    try:
     if fd.resolve()==image:raise RuntimeError('QEMU has this image open: '+str(image))
    except OSError:pass
  except (OSError,PermissionError):pass

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--image',type=Path,default=ROOT/'build/bleskernos-ata-user.img')
 p.add_argument('--usb',type=Path,help='also regenerate this USB image from the updated disk')
 p.add_argument('--master-volume',type=int,choices=range(101),metavar='0..100',help='explicitly update the saved master volume')
 a=p.parse_args();image=a.image.resolve();usb=a.usb.resolve() if a.usb else None
 offline(image)
 if usb:
  if usb==image:p.error('USB and partition image must be different')
  offline(usb)
 files={'/SYSTEM/DRIVERS/ATIR128.DVR':ROOT/'build/system/drivers/ATIR128.DVR',
        '/SYSTEM/PROGRAMS/PROCESSMANAGER.BEX':ROOT/'build/programs/processmanager.o',
        '/SYSTEM/PROGRAMS/PERFORMANCE_MONITOR.BEX':ROOT/'build/programs/performance_monitor.o',
        '/SYSTEM/SCREENS/SSPIPES.SCV':ROOT/'build/system/screensavers/ss_pipes.o',
        '/SYSTEM/SCREENS/SSBALLS.SCV':ROOT/'build/system/screensavers/ss_balls.o'}
 kernel=(ROOT/'build/kernel.bin').read_bytes();stage=(ROOT/'build/stage2.bin').read_bytes()
 if len(kernel)>1280*1024 or len(stage)!=4096:p.error('invalid boot payload size')
 for source in files.values():
  if not source.is_file():p.error('missing payload: '+str(source))
 temp=image.with_suffix(image.suffix+'.hardware-update')
 usbtemp=usb.with_suffix(usb.suffix+'.hardware-update') if usb else None
 if temp.exists() or (usbtemp and usbtemp.exists()):p.error('temporary update already exists')
 bfs4.check(image);shutil.copyfile(image,temp)
 try:
  e=Editor(temp)
  for dest,source in files.items():e.update(dest,source.read_bytes())
  sound_path='/SYSTEM/USER/CONFIG/SOUND.INI'
  sound_config=None
  if a.master_volume is not None:
   sound_config=set_master_volume(e.contents(sound_path),a.master_volume)
   e.update(sound_path,sound_config)
  e.finish()
  with temp.open('r+b') as f:
   f.seek(512);f.write(stage);f.seek(9*512);f.write(kernel.ljust(1280*1024,b'\0'))
   f.flush();os.fsync(f.fileno())
  bfs4.check(temp);e=Editor(temp)
  try:
   for dest,source in files.items():
    if e.contents(dest)!=source.read_bytes():raise ValueError('payload mismatch: '+dest)
   if sound_config is not None and e.contents(sound_path)!=sound_config:
    raise ValueError('sound configuration mismatch')
  finally:e.fd.close()
  if usb:
   subprocess.run([sys.executable,str(ROOT/'tools/build_usb_image.py'),str(usbtemp),str(temp),str(ROOT/'build/mbr.bin')],check=True)
   bfs4.check(usbtemp,boot_sectors=2048+bfs4.BOOT_SECTORS)
   with usbtemp.open('r+b') as f:os.fsync(f.fileno())
  # Recheck after preparing the copies in case an emulator was started meanwhile.
  offline(image)
  if usb:offline(usb)
  os.replace(temp,image)
  if usb:os.replace(usbtemp,usb)
  print('Updated kernel, ATI driver, monitors and screen savers; user configuration preserved:',image)
  if a.master_volume is not None:print('Saved master volume:',a.master_volume)
  if usb:print('USB image regenerated:',usb)
 finally:
  temp.unlink(missing_ok=True)
  if usbtemp:usbtemp.unlink(missing_ok=True)
if __name__=='__main__':main()
