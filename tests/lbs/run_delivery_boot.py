#!/usr/bin/env python3
"""Boot both MBR delivery formats with snapshots and require a real desktop."""
import argparse, concurrent.futures, json, subprocess, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--usb',type=Path,default=ROOT/'build/bleskernos-usb-user.img')
p.add_argument('--vmware',type=Path,default=ROOT/'build/bleskernos-vmware-user.vmdk')
a=p.parse_args()
out=ROOT/'tests/lbs/evidence/total/boot';out.mkdir(parents=True,exist_ok=True)
(out/'result.json').unlink(missing_ok=True)
def boot(kind,path,format):
    serial=out/(kind+'.log');serial.unlink(missing_ok=True)
    with (out/(kind+'-qemu.log')).open('w') as errors:
        proc=subprocess.Popen(['qemu-system-i386','-accel','kvm','-cpu','host','-m','256','-smp','2',
             '-vga','std','-drive',f'file={path.resolve()},format={format},if=ide,snapshot=on',
             '-display','none','-serial',f'file:{serial}','-no-reboot'],stderr=errors)
        start=time.monotonic()
        try:
            while time.monotonic()-start<180:
                text=serial.read_text(errors='replace') if serial.exists() else ''
                if proc.poll() is not None:raise RuntimeError(kind+' QEMU exited')
                if any(s in text for s in ['KERNEL PANIC','excepcion Ring 3','free invalido','[LBS:APP] FAIL']):
                    raise RuntimeError(kind+': '+text[-2000:])
                if ('[BKBOOT] DESKTOP RELEASED' in text or '[BKBOOT] QUEUE STARTUP SOUND' in text) and '[VISION:ENGINE] compositor y ventanas CPL=3' in text:
                    return {'format':kind,'status':'pass','seconds':round(time.monotonic()-start,2),
                            'method':'QEMU/KVM IDE snapshot, std VGA, MBR to Ring-3 desktop; not VMware hardware validation'}
                time.sleep(.2)
            raise TimeoutError(kind+' desktop startup')
        finally:
            proc.terminate()
            try:proc.wait(timeout=5)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
    jobs=[pool.submit(boot,'usb',a.usb,'raw'),pool.submit(boot,'vmware',a.vmware,'vmdk')]
    rows=[job.result() for job in jobs]
(out/'result.json').write_text(json.dumps({'status':'pass','boots':rows},indent=2)+'\n')
print('PASS: USB and VMDK MBR boot into Ring-3 desktop using disposable snapshots')
