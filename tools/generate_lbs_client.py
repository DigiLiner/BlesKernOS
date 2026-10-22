#!/usr/bin/env python3
"""Bind an existing native application's imports to their real LBS providers.

Only application references are renamed. The loader/host bridge keep the original
native syscall imports, so no dispatch slot can recurse into itself. Assembly tail
jumps preserve cdecl, callbacks, variadic calls and hidden struct-return arguments.
"""
import argparse
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    return subprocess.check_output([str(a) for a in args], text=True)


def symbols(path, dynamic=False, undefined=False):
    args = ['nm'] + (['-D'] if dynamic else [])
    args += ['--undefined-only' if undefined else '--defined-only', str(path)]
    return {line.split()[-1] for line in run(*args).splitlines() if line.strip()}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--application')
    p.add_argument('--source')
    p.add_argument('--output')
    p.add_argument('--destination')
    p.add_argument('--host-modules', nargs='*', default=[], help='also bridge modules opened explicitly at runtime')
    p.add_argument('--cc', default='gcc')
    p.add_argument('--ld', default='ld')
    a = p.parse_args()
    if a.application:
        if any([a.source, a.output, a.destination]): p.error('choose --application or explicit paths')
        config = json.loads((ROOT/'sdk/lbs/applications.json').read_text())[a.application]
    else:
        if not all([a.source, a.output, a.destination]): p.error('provide --source, --output and --destination')
        config = {'input': a.source, 'output': a.output, 'destination': a.destination}
    if Path(config['input']).resolve() == Path(config['output']).resolve():
        p.error('source and output must differ')
    source, output = ROOT/config['input'], ROOT/config['output']
    if 'bk_lbs_host_symbol' in symbols(source):
        raise RuntimeError(f'{source} already contains an LBS bridge; use its pre-runtime object')
    work = output.parent/(output.stem + '.client')
    work.mkdir(parents=True, exist_ok=True)
    providers = {}
    # NGS/FONT/MEDIA own their narrower API even if VISION also forwards it.
    order = ['SYSTEM','VISION','NGS','FONT','INPUT','SOUND','NETWORK','MEDIA','PRINT','CRYPTO','SECURITY','TINYGL','NGS3D','ARCHIVE','COMMAND']
    for module in order:
        for line in run('nm','-D','--defined-only',ROOT/f'build/system/libs/native/{module}.LBS').splitlines():
            parts=line.split()
            # A cdecl dispatch stub is a function, never a replacement for data.
            if len(parts)<3 or parts[-2] not in ('T','W'):continue
            symbol=parts[-1]
            if symbol not in ('bk_lbs_info', 'bk_lbs_live_resources'):
                providers[symbol] = module
    imports = sorted(symbols(source, undefined=True) & providers.keys())
    if not imports:
        raise RuntimeError(f'No LBS imports in {source}')
    modules = sorted({providers[n] for n in imports})
    slots = {n: i for i, n in enumerate(imports)}
    (work/'rename.txt').write_text('bleskernos_program_main bk_lbs_application_main\n' + ''.join(f'{n} __lbs_call_{n}\n' for n in imports))
    run('objcopy', '--redefine-syms='+str(work/'rename.txt'), source, work/'application.o')
    asm = ['.text']
    for symbol in imports:
        asm += [f'.globl __lbs_call_{symbol}', f'.type __lbs_call_{symbol},@function',
                f'__lbs_call_{symbol}:', f' jmp *__bk_lbs_app_slots+{slots[symbol]*4}',
                f'.size __lbs_call_{symbol},.-__lbs_call_{symbol}']
    asm += ['.section .note.GNU-stack,"",@progbits']
    (work/'dispatch.S').write_text('\n'.join(asm)+'\n')
    entry = ['#include <bleskernos_lbs.h>', '#include <bleskernos_api.h>',
             '#include <dlfcn.h>', 'extern void bk_lbs_application_main(void *);',
             f'void *__bk_lbs_app_slots[{len(imports)}];',
             'void bleskernos_program_main(void *arg) {',
             f' void *handles[{len(modules)}]={{0}};', ' unsigned loaded=0;',
             ' const char *failed="opening module";']
    for i, module in enumerate(modules):
        entry += [f' failed="{module}.LBS";', f' handles[{i}]=bk_lbs_open(failed,BK_LBS_ABI_MAJOR);',
                  f' if(!handles[{i}])goto fail;', ' loaded++;']
    for symbol in imports:
        entry += [f' failed="{symbol}";',
                  f' __bk_lbs_app_slots[{slots[symbol]}]=dlsym(handles[{modules.index(providers[symbol])}],failed);',
                  f' if(!__bk_lbs_app_slots[{slots[symbol]}])goto fail;']
    # Native applications can return with callbacks/threads still registered, or
    # exit through a syscall. Retain code until process VM teardown in all cases.
    entry += [' for(unsigned i=0;i<loaded;i++)if(bk_dl_pin(handles[i]))goto fail;',
              f' bk_sys_log("[LBS:APP] {config["destination"]} modules={",".join(modules)} symbols={len(imports)} ready\\n");',
              ' bk_lbs_application_main(arg);', ' return;', 'fail:',
              f' bk_sys_log("[LBS:APP] FAIL {config["destination"]}: ");',
              ' bk_sys_log(failed); bk_sys_log("\\n");',
              ' {const char *error=dlerror();if(error)bk_sys_log(error);bk_sys_log("\\n");}',
              ' while(loaded)dlclose(handles[--loaded]);', ' bk_exit(127);', '}']
    (work/'entry.c').write_text('\n'.join(entry)+'\n')
    host_modules = sorted(set(modules) | set(a.host_modules))
    run('python3', ROOT/'tools/generate_lbs_host.py', '--modules', *host_modules, '--output', work/'host.c')
    flags = ['-m32','-ffreestanding','-fno-builtin','-nostdlib','-nostdinc','-Os','-Wall','-Werror',
             '-fno-pic','-fno-pie','-fno-stack-protector','-fno-asynchronous-unwind-tables','-I'+str(ROOT/'sdk/include')]
    for file in ['entry.c','host.c','dispatch.S']:
        run(a.cc, *flags, '-c', work/file, '-o', work/(Path(file).stem+'.o'))
    run(a.ld, '-m','elf_i386','-r',work/'application.o',work/'entry.o',work/'dispatch.o',work/'host.o',
        ROOT/'build/sdk/lbs.o',ROOT/'build/sdk/libblesk_lbs_posix.a',ROOT/'build/sdk/libm.a',ROOT/'build/sdk/libblesk_lbs_native.a','-o',output)
    report = {'destination':config['destination'],'source':config['input'],'output':config['output'],
              'modules':modules,'imports':{s:providers[s] for s in imports},'process_lifetime_pin':True}
    if host_modules != modules: report['host_modules'] = host_modules
    output.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f'LBS client: {config["destination"]}: {len(imports)} imports, {len(modules)} modules')


if __name__ == '__main__':
    main()
