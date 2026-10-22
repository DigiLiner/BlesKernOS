"""Acceptance of real guest demo output, performance and CPU/GPU image parity."""
import io,re,json
from PIL import Image,ImageChops,ImageStat
from bfs4_update import Editor
from bfs4_journal import recover
def evaluate(text,out,image,common,min_fps):
    scenes=[]
    pattern=r'\[MESADEMOS\] START ([^\n]*) backend=(\w+).*?\[MESADEMOS\] RESULT backend=(\w+) frames=(\d+) seconds=([\d.]+) fps=([\d.]+) draws=(\d+) triangles=(\d+) uploads=(\d+) downloads=(\d+) fallbacks=(\d+) strict_failures=(\d+)'
    for m in re.finditer(pattern,text,re.S):
        scenes.append(dict(name=m[1],backend=m[3],frames=int(m[4]),seconds=float(m[5]),fps=float(m[6]),draws=int(m[7]),triangles=int(m[8]),uploads=int(m[9]),downloads=int(m[10]),fallbacks=int(m[11]),strict_failures=int(m[12])))
    comparisons=[];recovered=recover(image);editor=Editor(image)
    try:
        for name in ['gears','gloss','morph3d']:
            pair=[]
            for mode in ['gpu','cpu']:
                try:
                    data=editor.contents('/SYSTEM/USER/'+name+'-'+mode+'.ppm')
                    (out/(name+'-'+mode+'.ppm')).write_bytes(data)
                    picture=Image.open(io.BytesIO(data)).convert('RGB');picture.save(out/(name+'-'+mode+'.png'));pair.append(picture)
                except FileNotFoundError:break
            if len(pair)==2:
                diff=ImageChops.difference(*pair);mae=sum(ImageStat.Stat(diff).mean)/3
                def foreground(pic):
                    r,g,b=pic.split()
                    return ImageChops.lighter(ImageChops.lighter(r,g),b).point(lambda v:255 if v>24 else 0)
                masks=[foreground(pic) for pic in pair]
                nonblack=[mask.histogram()[255] for mask in masks]
                union=ImageChops.lighter(*masks)
                foreground_mae=sum(ImageStat.Stat(diff,union).mean)/3 if max(nonblack) else 255
                coverage=min(nonblack)/max(nonblack) if max(nonblack) else 0
                comparisons.append(dict(name=name,mean_absolute_error=mae,foreground_mae=foreground_mae,nonblack=nonblack,coverage_ratio=coverage,pass_image=mae<8 and foreground_mae<8 and min(nonblack)>500 and coverage>.85))
    finally:editor.fd.close()
    measured=[s for s in scenes if s['seconds']>=4]
    workload=text[text.find('[MESADEMOS:BOOT]'):] if '[MESADEMOS:BOOT]' in text else ''
    functional=len(scenes)==15 and len(comparisons)==3 and all(c['pass_image'] for c in comparisons) and '[MESADEMOS:BOOT] COMPLETE' in text and not re.search(r'\bFAIL\b|panic|Page Fault|LINUX:FAULT|GPU_STRICT|unsupported syscall',workload,re.I) and not re.search(r'panic|Page Fault|LINUX:FAULT',text,re.I)
    functional=functional and all(s['draws']>0 and s['triangles']>0 and s['downloads']==0 and s['fallbacks']==0 and s['strict_failures']==0 for s in measured if s['backend']=='mesa35')
    close_controls=json.loads((out/'close-controls.json').read_text())
    lifecycle=len(close_controls)==3 and all(c['returncode']==0 for c in close_controls) and text.count('[MESADEMOS] CLOSED')==15 and text.count('[MESADEMOS] CLOSE_CONFIRMED')==3
    native_lbs='[LBSTRI] ALL PASS' in text
    functional=functional and native_lbs and lifecycle and common["direct_scanout"]
    performance=functional and len(measured)==9 and all(s['fps']>=min_fps for s in measured if s['backend']!='software')
    return dict(device=common['device'],core_sha256=common['core_sha256'],package_manifest_sha256=common['package_manifest_sha256'],suite='mesa-demos',native_lbs_pass=native_lbs,direct_scanout=common['direct_scanout'],lifecycle_pass=lifecycle,recovered_journal_records=recovered,scenes=scenes,comparisons=comparisons,functional_pass=functional,performance_pass=performance,performance_threshold_fps=min_fps,**{'pass':functional and performance})
