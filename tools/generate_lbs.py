#!/usr/bin/env python3
"""Generate PIC forwarding functions from the existing, versioned SDK ABI.

Each import names a Ring 3 syscall trampoline in the opt-in host bridge.
No kernel address is discovered or called by these modules.
"""
import json,re
from pathlib import Path
root=Path(__file__).resolve().parents[1]
out=root/'build/sdk/lbs/generated';out.mkdir(parents=True,exist_ok=True)
manifest=json.loads((root/'sdk/lbs/manifest.json').read_text())
dependencies=json.loads((root/'sdk/lbs/dependencies.json').read_text())
legacy=json.loads((root/'sdk/lbs/legacy_exports.json').read_text())
header=(root/'sdk/include/bleskernos_api.h').read_text()
header=re.sub(r'/\*.*?\*/','',header,flags=re.S)
functions=set(re.findall(r'\b(bk_\w+)\s*\(',header))
functions-= {'bk_console_printf'} # variadic convenience belongs to the host
# Public declarations from included DTO headers.
functions.update(['bk_input_preference_get','bk_input_preference_set'])
# Forwarding avoids parsing signatures and preserves hidden struct-return args.
for module,prefixes in manifest.items():
 names=sorted(n for n in functions if any(n.startswith(p) for p in prefixes))
 # Keep ownership distinct for these subsystem-specific services.
 if module=='SYSTEM':names=[n for n in names if n!='bk_file_dialog_open']
 if module=='VISION':names=[n for n in names if not any(n.startswith(p) for p in ['bk_gui_image_','bk_gui_surface_','bk_gui_text_width','bk_gui_cursor_'])]
 names=sorted(set(names)|set(legacy.get(module,[])))
 lines=['.text']
 for n in names:
  lines += [f'.globl {n}',f'.type {n},@function',f'{n}:',
   '  call 1f','1: popl %eax','  addl $_GLOBAL_OFFSET_TABLE_+(.-1b), %eax',
   f'  movl __lbs_host_{n}@GOT(%eax), %eax','  jmp *%eax',f'.size {n},.-{n}']
 lines+=['.section .note.GNU-stack,"",@progbits']
 (out/f'{module}.S').write_text('\n'.join(lines)+'\n')
 (out/f'{module}.exports').write_text('\n'.join(names)+'\n')
 info=['#include <bleskernos_lbs.h>',
  f'const uint32_t bk_{module.lower()}_abi_1=BK_LBS_ABI_MAJOR;',
  f'static const bk_lbs_info_t info={{sizeof(info),BK_LBS_ABI_MAJOR,0,"{module}",BK_SYSCALL_ABI_VERSION}};',
  'const bk_lbs_info_t *bk_lbs_info(void){return &info;}',
  'static volatile uint32_t live_resources;',
  'uint32_t bk_lbs_live_resources(void){return __sync_fetch_and_add(&live_resources,0);}',
  'void __attribute__((visibility("hidden"))) bk_lbs_resource_acquire(void){__sync_fetch_and_add(&live_resources,1);}',
  'void __attribute__((visibility("hidden"))) bk_lbs_resource_release(void){__sync_fetch_and_sub(&live_resources,1);}']
 if dependencies[module]:
  for dep in dependencies[module]:info.append(f'extern const uint32_t bk_{dep.lower()}_abi_1;')
  info.append('const uint32_t *const bk_lbs_dependency_abis[]={'+','.join('&bk_'+d.lower()+'_abi_1' for d in dependencies[module])+'};')
 (out/f'{module}_info.c').write_text('\n'.join(info)+'\n')
policy=(root/'sdk/lbs/exports.map').read_text()
legacy_names=sorted({n for names in legacy.values() for n in names})
(out/'exports.map').write_text(policy.replace('/* LEGACY_EXPORTS */', '\n'.join('  '+n+';' for n in legacy_names)))
print('Generated LBS forwarding interfaces')
