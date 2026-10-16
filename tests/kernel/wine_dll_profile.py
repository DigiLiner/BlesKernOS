#!/usr/bin/env python3
"""Read pager ownership in a paused test VM, counting shared frames once."""
import argparse,json,re,socket,struct,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--name',default='dll-profile')
a=p.parse_args();out=a.out.resolve();v=json.loads((out/'vm.json').read_text())
pager=(ROOT/'kernel/mm/pager.c').read_text()
vfs=(ROOT/'kernel/drivers/core/vfs.c').read_text()
definitions=pager[pager.index('typedef struct file_chunk'):pager.index('static pager_region_t *regions;')]
definitions+=vfs[vfs.index('typedef enum {'):vfs.index('static vfs_page_reference_t *g_page_references;')]
keys=['region_size','region_next','pid','count','file','pages','page_size','frame','slot','source_frame','alive','source_ref','source_chunks','chunk_size','chunk_next','chunk_frames','reference_path','vfs_size','vfs_used','vfs_path','vfs_dirty','vfs_capacity','vfs_cache_size']
expressions=['sizeof(pager_region_t)','O(pager_region_t,next)','O(pager_region_t,pid)','O(pager_region_t,count)','O(pager_region_t,file)','O(pager_region_t,pages)','sizeof(pager_page_t)','O(pager_page_t,frame)','O(pager_page_t,slot)','O(pager_page_t,source_frame)','O(pager_page_t,alive)','O(file_source_t,reference)','O(file_source_t,chunks)','sizeof(file_chunk_t)','O(file_chunk_t,next)','O(file_chunk_t,frames)','O(struct vfs_page_reference,file)+O(vfs_file_t,path)','sizeof(vfs_file_t)','O(vfs_file_t,used)','O(vfs_file_t,path)','O(vfs_file_t,cache_dirty)','O(vfs_file_t,cache_capacity)','O(vfs_file_t,cache_size)']
source=out/'dll-offsets.c'
source.write_text('#include "kernel/include/pager.h"\n#include "kernel/include/vfs.h"\n#include "kernel/include/iso9660.h"\n'+definitions+'\nextern int printf(const char *,...);\n#define O(t,m) ((unsigned)__builtin_offsetof(t,m))\nint main(void){printf("'+' '.join(['%u']*len(keys))+'",'+','.join(expressions)+');}\n')
subprocess.run(['gcc','-m32','-I'+str(ROOT),str(source),'-o',str(out/'dll-offsets')],check=True)
layout=dict(zip(keys,map(int,subprocess.check_output([str(out/'dll-offsets')],text=True).split())))
symbols=subprocess.check_output(['nm',str(out/'kernel.elf')],text=True)
base=int(re.search(r'^([0-9a-f]+) b regions$',symbols,re.M)[1],16)
s=socket.socket(socket.AF_UNIX);s.settimeout(30);s.connect(v['sock']);f=s.makefile('rwb',buffering=0);f.readline()
def cmd(name,args=None):
    f.write((json.dumps({'execute':name,'arguments':args or {}})+'\n').encode())
    while True:
        r=json.loads(f.readline())
        if 'error' in r:raise RuntimeError(r)
        if 'return' in r:return r['return']
scratch=out/(a.name+'-scratch.bin')
def memory(address,size):
    assert 0<address<v['ram']*1048576 and size<=v['ram']*1048576-address
    cmd('pmemsave',{'val':address,'size':size,'filename':str(scratch)})
    data=scratch.read_bytes();scratch.unlink();return data
def word(data,offset=0):return struct.unpack_from('<I',data,offset)[0]
cmd('qmp_capabilities');cmd('stop')
rows={};sources={};private=set();clean=set();seen=set();handles=[]
try:
    files=int(re.search(r'^([0-9a-f]+) b g_files$',symbols,re.M)[1],16)
    rawfiles=memory(files,128*layout['vfs_size'])
    for index in range(128):
        raw=rawfiles[index*layout['vfs_size']:(index+1)*layout['vfs_size']]
        if raw[layout['vfs_used']]:handles.append({'fd':index,'path':raw[layout['vfs_path']:layout['vfs_path']+260].split(b'\0')[0].decode(errors='replace'),'dirty':bool(raw[layout['vfs_dirty']]),'capacity':word(raw,layout['vfs_capacity']),'size':word(raw,layout['vfs_cache_size'])})
    pointer=word(memory(base,4))
    while pointer:
        assert pointer not in seen and len(seen)<10000;seen.add(pointer)
        header=memory(pointer,layout['region_size']);pid=word(header,layout['pid']);count=word(header,layout['count']);file=word(header,layout['file'])
        path='[anonymous]'
        if file:
            if file not in sources:
                source=memory(file,max(layout['source_ref'],layout['source_chunks'])+4)
                ref=word(source,layout['source_ref']);path=memory(ref+layout['reference_path'],260).split(b'\0')[0].decode(errors='replace')
                sources[file]=path
            path=sources[file]
        key=(pid,path);row=rows.setdefault(key,{'pid':pid,'path':path,'regions':0,'reserved_pages':0,'private_pages':0,'source_mappings':0,'swapped_pages':0})
        row['regions']+=1;body=memory(pointer+layout['pages'],count*layout['page_size'])
        for i in range(count):
            raw=body[i*layout['page_size']:(i+1)*layout['page_size']]
            if not raw[layout['alive']]:continue
            row['reserved_pages']+=1;frame=word(raw,layout['frame']);borrow=word(raw,layout['source_frame'])
            if frame:private.add(frame);row['private_pages']+=1
            if borrow:clean.add(borrow);row['source_mappings']+=1
            if word(raw,layout['slot']):row['swapped_pages']+=1
        pointer=word(header,layout['region_next'])
finally:
    cmd('cont');s.close();scratch.unlink(missing_ok=True)
result={'method':'paused physical pager metadata; private frames and mapped source frames deduplicated globally; excludes unmapped cache frames, heap and non-pager ELF pages','layout':layout,'handles':handles,'private_frames':len(private),'mapped_source_frames':len(clean),'rows':sorted(rows.values(),key=lambda r:(-r['private_pages'],r['pid'],r['path']))}
(out/(a.name+'.json')).write_text(json.dumps(result,indent=2))
print(json.dumps({k:result[k] for k in ('private_frames','mapped_source_frames')}))
