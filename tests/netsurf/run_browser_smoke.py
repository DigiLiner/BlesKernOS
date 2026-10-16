#!/usr/bin/env python3
"""Boot the actual browser against a local UTF-8/font fixture and check scrolling."""
import argparse
import http.server
import gzip
import re
import json
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time
from PIL import Image, ImageChops
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,default=Path('/tmp/blesk-netsurf-smoke'))
p.add_argument('--large',action='store_true',help='Serve a gzip HTML body larger than the former 80 KiB decode limit')
p.add_argument('--chunked',action='store_true')
p.add_argument('--bad-gzip',action='store_true',help='Verify a failed gzip GET retries with identity')
p.add_argument('--slow',action='store_true',help='Exercise the secondary-resource deadline')
p.add_argument('--url',help='Optional live URL instead of the local fixture')
p.add_argument('--local',action='store_true',help='Load the fixture from file:///SYSTEM/USER/NSLOCAL.HTM')
p.add_argument('--ui',action='store_true',help='Exercise settings, persistence, resize, and the local-file selector')
p.add_argument('--baseline',action='store_true',help='Run the browser already in the base image for comparison')
a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
html='''<!doctype html><meta charset="utf-8"><title>NetSurf — fuentes del OS</title>
<style>body{font-family:"DejaVu Sans";font-size:16px;padding:12px;color:#203040}
h1{font-size:28px;color:#163a70} .serif{font-family:"DejaVu Serif";font-size:20px}
.mono{font-family:"DejaVu Sans Mono";font-size:15px} .spaced{letter-spacing:2px;word-spacing:3px}
p{margin:12px 0} a{color:#1240a0}</style>
<h1>NetSurf: fuentes del sistema</h1><p>¡Hola! Español: áéíóú, ñ, ü, ¿qué tal? 25 € — “Texto” …</p>
<p class="serif">DejaVu Serif: lectura proporcional con acentos.</p>
<p><b>Negrita legible</b> y <i>cursiva con suavizado</i>.</p>
<p class="mono">Monospace: iii WWW 0123456789</p>
<p class="spaced">Espaciado: ñ € — texto.</p>
<form><input value="Español: búsqueda"><button>Buscar</button></form>
'''+''.join(f'<p>Bloque {i}: texto de prueba para desplazar la página y comprobar los saltos de línea sin perder caracteres.</p>' for i in range(12))
if a.ui:html=html.replace('<h1>', '<div style="height:25vh;width:25vw;background:#93b5d7"></div><div><span style="display:inline-block;box-sizing:border-box;padding:8px 12px;background:#84a880">Acceder</span></div><h1>',1)
if a.large:html+='<!--'+('decode buffer regression '*6000)+'-->'
if a.slow:html+='<link rel="stylesheet" href="/slow.css">'
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path=='/slow.css':time.sleep(5)
        data=html.encode()
        compressed='gzip' in self.headers.get('Accept-Encoding','')
        if compressed:data=b'broken gzip stream' if a.bad_gzip else gzip.compress(data)
        self.send_response(200);self.send_header('Content-Type','text/html; charset=utf-8')
        if compressed:self.send_header('Content-Encoding','gzip')
        self.send_header('Transfer-Encoding','chunked') if a.chunked else self.send_header('Content-Length',str(len(data)))
        self.end_headers()
        try:
            if a.chunked:
                for offset in range(0,len(data),127):
                    chunk=data[offset:offset+127];self.wfile.write(f'{len(chunk):x}\r\n'.encode()+chunk+b'\r\n')
                self.wfile.write(b'0\r\n\r\n')
            else:self.wfile.write(data)
        except (BrokenPipeError,ConnectionResetError):pass
    def log_message(self,*args):pass
server=http.server.ThreadingHTTPServer(('0.0.0.0',0),Handler)
threading.Thread(target=server.serve_forever,daemon=True).start()
subprocess.run(['make','netsurf','build/stage2.bin','build/system/services/vision.o','build/system/drivers/NETSTACK.DVR'],cwd=ROOT,check=True,stdout=subprocess.DEVNULL)
image=out/'browser.img';sock=out/'qmp.sock';sock.unlink(missing_ok=True)
subprocess.run(['cp','--reflink=auto',str(ROOT/'build/bleskernos-ata-user.img'),str(image)],check=True)
with image.open('r+b') as disk:
    disk.seek(512);disk.write((ROOT/'build/stage2.bin').read_bytes())
    disk.seek(9*512);disk.write((ROOT/'build/kernel.bin').read_bytes().ljust(1280*1024,b'\0'))
e=Editor(image)
e.update('/SYSTEM/DRIVERS/NETSTACK.DVR',(ROOT/'build/system/drivers/NETSTACK.DVR').read_bytes())
e.update('/SYSTEM/SERVICES/VISION.BEX',(ROOT/'build/system/services/vision.o').read_bytes())
if not a.baseline:
    e.update('/SYSTEM/PROGRAMS/NETSURF.BEX',(ROOT/'build/programs/netsurf.o').read_bytes())
e.update('/SYSTEM/USER/NSLOCAL.HTM',(html+'<link rel="stylesheet" href="NSSTYLE.CSS">').encode())
e.update('/SYSTEM/USER/NSSTYLE.CSS',b'body { background:#f8fbff }')
if a.slow:e.update('/SYSTEM/USER/CONFIG/NETSURF.INI',b'resources=2000\n')
url=a.url or ('file:///SYSTEM/USER/NSLOCAL.HTM' if a.local else f'http://10.0.2.2:{server.server_port}/')
e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',f'4|1|1|10|0|1|3000|Browser test|/SYSTEM/PROGRAMS/NETSURF.BEX|{url}|Browser test\n'.encode());e.finish()
log=out/'guest.log';log.unlink(missing_ok=True)
proc=subprocess.Popen(['qemu-system-i386','-accel','kvm','-cpu','host','-smp','1','-m','128','-drive',f'file={image},format=raw,if=ide','-vga','vmware','-display','none','-serial',f'file:{log}','-netdev','user,id=net0','-device','rtl8139,netdev=net0','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot','-no-shutdown'],stderr=(out/'qemu.log').open('w'))
try:
    deadline=time.monotonic()+80
    while time.monotonic()<deadline:
        text=log.read_text(errors='replace') if log.exists() else ''
        assert '[TASK] excepcion' not in text and '[PANIC]' not in text,text[-2000:]
        if '[NETSURF][PERF]' in text:break
        if proc.poll() is not None:raise RuntimeError('QEMU exited')
        time.sleep(.2)
    assert '[NETSURF][PERF]' in text,text[-3000:]
    time.sleep(2)
    s=socket.socket(socket.AF_UNIX);s.settimeout(10);s.connect(str(sock));f=s.makefile('rwb',buffering=0);f.readline()
    def cmd(name,args=None):
        f.write((json.dumps({'execute':name,'arguments':args or {}})+'\n').encode())
        while True:
            result=json.loads(f.readline())
            if 'error' in result:raise RuntimeError(result)
            if 'return' in result:return result['return']
    cmd('qmp_capabilities')
    def screenshot(name):
        ppm=out/(name+'.ppm');cmd('screendump',{'filename':str(ppm)});im=Image.open(ppm).convert('RGB');im.save(out/(name+'.png'));return im
    before=screenshot('fonts')
    pointer=[before.width//2,before.height//2]
    def move(x,y):
        while pointer != [x,y]:
            dx=max(-100,min(100,x-pointer[0]));dy=max(-100,min(100,y-pointer[1]))
            cmd('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':dx}},{'type':'rel','data':{'axis':'y','value':dy}}]})
            pointer[0]+=dx;pointer[1]+=dy
            time.sleep(.08)
    def click(x,y):
        move(x,y)
        cmd('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':True}}]})
        time.sleep(.05)
        cmd('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':False}}]})
        time.sleep(.3)
    if a.ui:
        click(250,90);time.sleep(1);screenshot('settings')
        click(403,409);time.sleep(1);screenshot('settings-saved')
        # Settings are separate from the OS internet defaults.
        disk=Editor(image)
        saved=disk.contents('/SYSTEM/USER/CONFIG/NETSURF.INI')
        assert b'resources=' in saved and b'home=' in saved, 'Settings not saved'
        click(220,90);time.sleep(1);screenshot('open-local')
        cmd('send-key',{'keys':[{'type':'qcode','data':'esc'}]});time.sleep(1.5)
        def viewport_marker_height(im):
            return sum(any(im.getpixel((x,y))==(0x93,0xb5,0xd7) for x in range(80,600,20)) for y in range(im.height))
        button_rows=[y for y in range(before.height) if any(before.getpixel((x,y))==(0x84,0xa8,0x80) for x in range(80,300,10))]
        assert button_rows and 30<=len(button_rows)<=45, 'Automatic border-box button wrapped its label'
        old_marker=viewport_marker_height(before)
        old_width=max(sum(before.getpixel((x,y))==(0x93,0xb5,0xd7) for x in range(before.width)) for y in range(180,280))
        assert 169<=old_width<=172, f'25vw has wrong width: {old_width}'
        assert 75<=old_marker<=78, f'25vh has wrong initial height: {old_marker}'
        move(763,494)
        cmd('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':True}}]})
        time.sleep(.3)
        move(643,534)
        time.sleep(.3)
        cmd('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':False}}]})
        time.sleep(2)
        resized=screenshot('resized')
        new_marker=viewport_marker_height(resized)
        new_width=max(sum(resized.getpixel((x,y))==(0x93,0xb5,0xd7) for x in range(resized.width)) for y in range(180,280))
        assert 28<=old_width-new_width<=32, f'25vw did not follow width: {old_width}->{new_width}'
        assert 8<=new_marker-old_marker<=12, f'25vh did not follow viewport height: {old_marker}->{new_marker}'
        # Compare scrolling at the new dimensions.
        before=resized
        click(400,320)

    # PS/2 uses relative input. The initial cursor is centered over the page;
    # click the document to remove focus from the address textbox first.
    click(400,320)
    cmd('send-key',{'keys':[{'type':'qcode','data':'pgdn'}]})
    time.sleep(2)
    after=screenshot('scrolled')
    changed=sum(pixel!=(0,0,0) for pixel in ImageChops.difference(before,after).getdata())
    if not a.url:assert changed>500, 'Page did not scroll'
    cmd('send-key',{'keys':[{'type':'qcode','data':'f5'}]})
    reload_deadline=time.monotonic()+(60 if a.url else 20)
    while time.monotonic()<reload_deadline:
        text=log.read_text(errors='replace')
        if sum('[NETSURF][PERF]' in line and line.endswith('\n') for line in text.splitlines(keepends=True))>=2:break
        time.sleep(.1)
    assert text.count('[NETSURF][PERF]')>=2, 'Reload did not finish'
    assert '[TASK] excepcion' not in text and '[PANIC]' not in text
    if a.large and not a.local and not a.bad_gzip:assert 'gzip=1' in text, 'Compressed large page did not decode'
    if a.bad_gzip:assert 'gzip=0' in text and 'req=2' in text, 'Identity retry missing'
    if a.slow:
        totals=[int(n) for n in re.findall(r'\[NETSURF\]\[PERF\] total=(\d+)',text)]
        assert max(totals)<6000, 'Secondary resource deadline did not bound loading'
    result={'passed':True,'baseline':a.baseline,'local':a.local,'url':url,'scrolled_pixels':changed,'performance':[line for line in text.splitlines() if '[NETSURF][PERF]' in line]}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
    cmd('quit');proc.wait(timeout=5)
finally:
    if proc.poll() is None:proc.terminate();proc.wait(timeout=5)
    server.shutdown();sock.unlink(missing_ok=True);image.unlink(missing_ok=True)
