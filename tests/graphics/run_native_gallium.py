#!/usr/bin/env python3
"""Run the real native Gallium probe/glmark2 in a disposable KVM/VirGL VM."""
from pathlib import Path
import argparse,hashlib,json,os,re,shutil,subprocess,sys,time
from PIL import Image
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from bfs4_update import Editor
from run_qemu_virtio import graphics_environment
from run_qemu_graphics_validation import qmp,capture
def run(args):subprocess.run([str(x) for x in args],cwd=ROOT,check=True)
def parse_log(text):
    names='build|shading|jellyfish|refract|bump'
    scenes=[]
    for match in re.finditer(r'\[('+names+r')\](.*?)(?=\[(?:'+names+r')\]|\Z)',text,re.S):
        fps=re.search(r'FPS:\s*(\d+)',match[2])
        frame=re.search(r'FrameTime:\s*([\d.]+)',match[2])
        scenes.append({'name':match[1],'fps':int(fps[1]) if fps else None,
            'frame_ms':float(frame[1]) if frame else None,
            'unsupported':bool(re.search(r'Unsupported|Set up failed',match[2]))})
    return scenes
def probe_image_ok(out,name='probe'):
    path=next((p for p in [out/(name+'.png'),out/(name+'.ppm')] if p.is_file()),None)
    if not path:return False
    with Image.open(path) as picture:
        red=blue=red_y=blue_y=0
        rgb=picture.convert('RGB')
        for y in range(rgb.height):
            for r,g,b in (rgb.getpixel((x,y)) for x in range(rgb.width)):
                if r>240 and g<15 and b<15:red+=1;red_y+=y
                if b>240 and g<15 and r<15:blue+=1;blue_y+=y
        return red>50000 and blue>50000 and red_y/red<blue_y/blue
def gpu_sample(pid):
    clients={}
    try:
        for path in Path('/proc',str(pid),'fdinfo').iterdir():
            data=path.read_text()
            client=re.search(r'drm-client-id:\s*(\d+)',data)
            render=re.search(r'drm-engine-render:\s*(\d+)\s+ns',data)
            if client and render:clients[client[1]]=int(render[1])
    except (FileNotFoundError,PermissionError):pass
    return sum(clients.values()) if clients else None
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pacing-trace',action='store_true',help='buffer native desktop submission interval histograms (PIT resolution, not monitor refreshes)')
    p.add_argument('--pacing-only',action='store_true',help='diagnostic GPU demo runs without software references; cannot pass full acceptance')
    p.add_argument('--driver-pacing-trace',action='store_true',help='log long virtqueue waits for diagnosis')
    p.add_argument('--mesa-trace',action='store_true',help='diagnostic legacy IO timing')
    p.add_argument('--demo-scene',type=int,choices=[1,2,3],help='diagnostic single scene; cannot pass full acceptance')
    p.add_argument('--mesa-demos',action='store_true',help='run native Mesa demo comparison suite')
    p.add_argument('--base',type=Path,default=ROOT/'build/bleskernos-ata-user.img')
    p.add_argument('--out',type=Path,default=ROOT/'build/native-gallium/validation')
    p.add_argument('--seconds',type=int,default=150)
    p.add_argument('--min-fps',type=int,default=30,help='performance gate for every selected scene at 640x480')
    p.add_argument('--device',choices=['virgl','svga','virtio'],default='virgl')
    p.add_argument('--display',choices=['sdl','gtk'],default='gtk',help='host GL window backend')
    p.add_argument('--benchmarks',help='diagnostic replacement arguments; acceptance still requires all five scenes')
    p.add_argument('--expect-unavailable',action='store_true',help='verify rejection without a GPU renderer; does not approve installation')
    a=p.parse_args();out=a.out.resolve()
    if a.pacing_only and not (a.mesa_demos and a.pacing_trace):p.error('--pacing-only requires --mesa-demos --pacing-trace')
    if a.demo_scene and not a.mesa_demos:p.error('--demo-scene requires --mesa-demos')
    if out.exists() and any(out.iterdir()):p.error('use a new output directory; existing evidence is preserved')
    out.mkdir(parents=True,exist_ok=True)
    if a.min_fps<1:p.error('--min-fps must be positive')
    image=out/'test.img'
    if image.resolve()==a.base.resolve():raise ValueError('Use a disposable image')
    package=ROOT/'build/native-gallium/package'
    run(['make','-j3','VISION_PACING_TRACE='+('1' if a.pacing_trace else ''),
        'VIRTIO_GPU_PACING_TRACE='+('1' if a.driver_pacing_trace else ''),
        'build/kernel.bin','build/stage2.bin','build/system/drivers/VIRTIOGPU.DVR','build/system/services/vision.o'])
    core={name:(ROOT/name).read_bytes() for name in [
        'build/kernel.bin','build/stage2.bin','build/system/drivers/VIRTIOGPU.DVR','build/system/services/vision.o']}
    package_digest=hashlib.sha256((ROOT/'build/native-gallium/manifest.json').read_bytes()).hexdigest()
    benchmark_args=['-DBLES_GL_TEST_ARGS='+json.dumps(a.benchmarks)] if a.benchmarks else []
    if a.mesa_trace:benchmark_args.append('-DBLES_MESA_TRACE=" --bkg-trace"')
    if a.demo_scene:benchmark_args.append('-DBLES_MESA_SCENE='+str(a.demo_scene))
    if a.pacing_only:benchmark_args.append('-DBLES_MESA_PACING_ONLY=1')
    run(['gcc','-m32','-march=pentium3','-mfpmath=387','-mno-sse','-mno-sse2','-ffreestanding','-fno-builtin','-nostdlib','-nostdinc','-Os',*benchmark_args,
        '-fno-pic','-fno-pie','-fno-stack-protector','-I.','-Ikernel/libc/include','-c',ROOT/('tests/graphics/native_mesa_demos_boot.c' if a.mesa_demos else 'tests/graphics/native_gallium_boot.c'),'-o',out/'boot.o'])
    run(['cp','--reflink=auto',a.base,image])
    with image.open('r+b') as f:
        f.seek(512);f.write(core['build/stage2.bin'])
        kernel=core['build/kernel.bin']
        if len(kernel)>1280*1024:raise ValueError('kernel reservation exceeded')
        f.seek(9*512);f.write(kernel.ljust(1280*1024,b'\0'))
    e=Editor(image)
    for f in sorted(package.rglob('*')):
        if f.is_file():
            path='/'+str(f.relative_to(package));data=f.read_bytes()
            try:unchanged=e.contents(path)==data
            except FileNotFoundError:unchanged=False
            if not unchanged:e.mkdir(str(Path(path).parent));e.update(path,data)
    e.update('/SYSTEM/DRIVERS/VIRTIOGPU.DVR',core['build/system/drivers/VIRTIOGPU.DVR'])
    e.update('/SYSTEM/SERVICES/VISION.BEX',core['build/system/services/vision.o'])
    e.update('/SYSTEM/PROGRAMS/GALLBOOT.BEX',(out/'boot.o').read_bytes())
    e.update('/SYSTEM/USER/CONFIG/STARTUP.INI',b'4|1|1|10|0|1|30000|Native Gallium|/SYSTEM/PROGRAMS/GALLBOOT.BEX||Gallium validation\n')
    e.update('/SYSTEM/USER/CONFIG/SCREENSV.INI',b'enabled=0\ntimeout=3600\npath=/SYSTEM/SCREENS/SSPIPES.SCV\n');e.finish()
    sock=out/'qmp.sock';sock.unlink(missing_ok=True)
    # QMP sockets have a short platform path limit, so use a temporary short name.
    sock=Path('/tmp/bkgallium-'+str(os.getpid())+'.sock')
    serial=out/'serial.log';serial.unlink(missing_ok=True)
    device=['-device','virtio-vga-gl,ioeventfd=on'] if a.device=='virgl' else ['-vga','vmware' if a.device=='svga' else 'virtio']
    display=(a.display+',gl=on'+(',zoom-to-fit=off,show-cursor=on' if a.display=='gtk' else '')) if a.device=='virgl' else 'none'
    command=['qemu-system-i386','-enable-kvm','-cpu','host','-smp','1','-m','768',
        '-drive',f'file={image},format=raw,if=ide',*device,'-display',display,
        '-serial',f'file:{serial}','-qmp',f'unix:{sock},server=on,wait=off','-no-reboot','-no-shutdown']
    (out/'command.json').write_text(json.dumps(command,indent=2)+'\n')
    env=graphics_environment('virtio-vga-gl') if a.device=='virgl' else os.environ.copy()
    (out/'host_environment.json').write_text(json.dumps({k:env[k] for k in ['DISPLAY','LD_PRELOAD','vblank_mode'] if k in env},indent=2)+'\n')
    proc=subprocess.Popen(command,cwd=ROOT,env=env,stdout=(out/'qemu.stdout').open('w'),stderr=(out/'qemu.stderr').open('w'))
    start=time.monotonic();captured=set();capture_ready={};gpu_samples=[];close_controls=[]
    try:
        while proc.poll() is None and time.monotonic()-start<a.seconds:
            text=serial.read_text(errors='replace') if serial.exists() else ''
            gpu_samples.append({'seconds':time.monotonic()-start,'render_ns':gpu_sample(proc.pid),
                'scene':(re.findall(r'\[(build|shading|jellyfish|refract|bump)\]',text) or ['boot'])[-1]})
            # Capture the VM window by PID; its GPU scanout may not support screendump.
            for marker,name in [('GPU frame ready','probe'),('[jellyfish]','jellyfish'),('[refract]','refract'),
                ('all native applications exited','cleanup'),('REOPENED_FRAME','probe-reopened'),('[LBSTRI] FRAME READY','lbs-triangle')]:
                if marker=='REOPENED_FRAME' and text.count('GPU frame ready')>=2:marker='GPU frame ready'
                if marker in text and name not in captured:
                    ready=capture_ready.setdefault(name,time.monotonic()+1)
                    if time.monotonic()<ready:continue
                    try:
                        # capture() uses out/qmp.sock; the GL fallback captures by PID.
                        capture(out,proc.pid,name)
                    except Exception as err:print('Capture:',err)
                    captured.add(name)
            if a.mesa_demos:
                for match in re.finditer(r'\[MESADEMOS:BOOT\] RUN ([\w.]+) (\w+)',text):
                    if match[2] not in ('fixedgpu','fixedcpu'):continue
                    segment=text[match.end():].split('[MESADEMOS:BOOT] RUN')[0]
                    if '[MESADEMOS] SNAPSHOT_READY' not in segment:continue
                    label='demo-'+match[1]+'-'+match[2]
                    if label not in captured and time.monotonic()>capture_ready.setdefault(label,time.monotonic()+1):
                        try:capture(out,proc.pid,label)
                        except Exception as err:print('Capture:',err)
                        captured.add(label)
                        if match[2]=='fixedcpu':
                            def hmp(command):return qmp(sock,'human-monitor-command',{'command-line':command})
                            for _ in range(35):hmp('mouse_move -40 -40');time.sleep(.05)
                            mx=my=0
                            while (mx,my)!=(669,48):
                                dx=max(-35,min(35,669-mx));dy=max(-35,min(35,48-my))
                                hmp(f'mouse_move {dx} {dy}');mx+=dx;my+=dy;time.sleep(.05)
                            hmp('mouse_button 1');time.sleep(.2);hmp('mouse_button 0')
                            close_controls.append({'scene':label,'returncode':0,'input':'HMP relative mouse, calibrated to corner'})
                if '[MESADEMOS:BOOT] COMPLETE' in text or '[MESADEMOS] FAIL' in text or '[MESADEMOS:BOOT] FAIL' in text:break
            if ('glmark2 Score:' in text and 'desktop alive after cleanup' in text) or re.search(r'\[GALLIUM:BOOT\] FAIL|\[GLMARK2:NATIVE\] FAIL|\[GALLIUM:PROBE\] FAIL|unrecognized option',text):break
            if a.device!='virgl' and '[GALLIUM:PROBE] FAIL' in text:break
            time.sleep(1)
    finally:
        try:qmp(sock,'quit')
        except Exception:proc.terminate()
        try:proc.wait(timeout=10)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
        sock.unlink(missing_ok=True)
    text=serial.read_text(errors='replace') if serial.exists() else ''
    (out/'close-controls.json').write_text(json.dumps(close_controls,indent=2)+'\n')
    (out/'host_gpu.json').write_text(json.dumps(gpu_samples,indent=2)+'\n')
    result={'device':a.device,'transport':'inprocess','probe_pass':'[GALLIUM:PROBE] PASS' in text,
        'core_sha256':{name:hashlib.sha256(data).hexdigest() for name,data in core.items()},
        'package_manifest_sha256':package_digest,
        'probe_image_pass':probe_image_ok(out),
        'reopened_image_pass':probe_image_ok(out,'probe-reopened'),
        'lifecycle_pass':'desktop alive after cleanup' in text and text.count('[GALLIUM:PROBE] PASS')>=2,
        'process_failures':re.findall(r'\[LINUX:EXIT\][^\n]*name=(?:GLMARK2|GALLCHECK|GALLTEST)\.ELF[^\n]*status=(?!0\b)[^\n]*',text),
        'renderer':re.findall(r'\[GALLIUM:PROBE\] renderer=(.*)',text),
        'shader_fbo_pass':'[GPU-PROBE] PASS real shader and FBO readback' in text,
        'glmark_score':re.findall(r'glmark2 Score:\s*(\d+)',text),
        'scenes':parse_log(text),
        'native_frames':re.findall(r'\[GALLIUM:NATIVE\].*',text),
        'direct_scanout':'[VIRTIOGPU:SCANOUT] GPU direct' in text and '[VIRTIOGPU:SCANOUT] CPU fallback' not in text,
        'elapsed_seconds':time.monotonic()-start}
    if a.mesa_demos:
        from native_mesa_results import evaluate
        result=evaluate(text,out,image,result,a.min_fps)
        from native_frame_pacing import parse_pacing,pacing_gate
        result['desktop_pacing']=parse_pacing(text)
        result['diagnostic_only']=bool(a.pacing_only or a.demo_scene)
        if a.pacing_trace:
            result['pacing_pass']=pacing_gate(result['desktop_pacing'],mesa_demos=True)
            result['pass']=result['pass'] and result['pacing_pass']
        if result['diagnostic_only']:result['pass']=False
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
        if not result['pass']:sys.exit(1)
        return
    workload=text[text.find('[GALLIUM:BOOT]'):] if '[GALLIUM:BOOT]' in text else ''
    result['functional_pass']=result['probe_pass'] and result['probe_image_pass'] and result['reopened_image_pass'] and result['lifecycle_pass'] and not result['process_failures'] and result['shader_fbo_pass'] and result['direct_scanout'] and bool(result['glmark_score']) and len(result['scenes'])==5 and all(
        s['fps'] and not s['unsupported'] for s in result['scenes']) and not re.search(r'\bFAIL(?:ED)?\b|unsupported syscall',workload,re.I) and not re.search(r'panic|Page Fault',text,re.I)
    result['performance_threshold_fps']=a.min_fps
    from native_frame_pacing import parse_pacing,pacing_gate
    result['desktop_pacing']=parse_pacing(text)
    result['performance_pass']=result['functional_pass'] and all(s['fps']>=a.min_fps for s in result['scenes'])
    result['pass']=result['functional_pass'] and result['performance_pass']
    if a.pacing_trace:
        result['pacing_pass']=pacing_gate(result['desktop_pacing'])
        result['pass']=result['pass'] and result['pacing_pass']
    result['expected_unavailable_pass']=bool(a.expect_unavailable and '[GALLIUM:PROBE] FAIL create' in text and
        '[GPU-PROBE] context FAILED' in text and not result['renderer'] and not result['native_frames'] and
        not re.search(r'panic|Page Fault',text,re.I))
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
    if not (result['expected_unavailable_pass'] if a.expect_unavailable else result['pass']):sys.exit(1)
if __name__=='__main__':main()
