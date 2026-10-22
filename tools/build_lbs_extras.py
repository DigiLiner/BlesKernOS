#!/usr/bin/env python3
"""Build optional native extras against the current LBS SDK, without embedding renderers."""
import argparse,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--only',nargs='*');a=p.parse_args()
 entries=json.loads((ROOT/'sdk/lbs/extra_applications.json').read_text());built=set()
 for name,c in entries.items():
  if a.only and name not in a.only:continue
  folder=ROOT/'extras'/c['project']
  if not folder.is_dir():raise FileNotFoundError(folder)
  recipe=(c['project'],c['target'])
  if recipe not in built:
   subprocess.run(['make','-j2','-C',str(folder),c['target'],*c['arguments']],check=True);built.add(recipe)
  source=folder/c['input'];output=ROOT/'build/lbs/extras'/name
  if c.get('private_runtime'):
   # Keep this port's FILE/socket/allocator ABI isolated from the LBS loader.
   # Rename definitions and their references together, before adding the bridge.
   def definitions(path):
    return {line.split()[-1] for line in subprocess.check_output(['nm','-g','--defined-only',str(path)],text=True).splitlines() if len(line.split())>=3}
   runtime=set()
   for archive in ['libblesk_lbs_posix.a','libblesk_lbs_native.a','libm.a']:
    runtime.update(definitions(ROOT/'build/sdk'/archive))
   conflicts=(definitions(source)&runtime)-{'bleskernos_program_main','bk_lbs_application_main','main'}
   work=ROOT/'build/lbs/extras/private-runtime';work.mkdir(parents=True,exist_ok=True)
   rename=work/(name+'.map');rename.write_text(''.join(n+' fc_private_'+n+'\n' for n in sorted(conflicts)))
   isolated=work/name
   subprocess.run(['objcopy','--redefine-syms='+str(rename),str(source),str(isolated)],check=True)
   source=isolated
  subprocess.run(['python3',str(ROOT/'tools/generate_lbs_client.py'),
                  '--source',str(source),'--output',str(output),
                  '--destination','/SYSTEM/PROGRAMS/'+name,*(['--host-modules',*c['host_modules']] if c.get('host_modules') else [])],check=True)
 subprocess.run(['python3',str(ROOT/'tests/lbs/audit_extras.py')],check=True)
if __name__=='__main__':main()
