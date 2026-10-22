#!/usr/bin/env python3
"""Document the exports that are actually present in the built LBS objects."""
from pathlib import Path
import json,subprocess
ROOT=Path(__file__).resolve().parents[1]
names=list(json.loads((ROOT/'sdk/lbs/dependencies.json').read_text()))
lines=['# Catálogo de exportaciones LBS ABI 1','',
 'Generado de `nm -D --defined-only` sobre los ELF construidos. Una exportación',
 'OpenGL no constituye una promesa de aceleración: consultar capacidades y estadísticas.',
 'Las firmas nativas están en `sdk/include/bleskernos_api.h`; las extensiones,',
 'en `bleskernos_*lbs*.h`, `bleskernos_ngs.h` y `bleskernos_ngs3d.h`.','',
 'Comunes: `bk_lbs_info`, `bk_lbs_live_resources`, marcador `bk_<nombre>_abi_1`',
 'y tabla de marcadores importados `bk_lbs_dependency_abis` cuando corresponde.','']
for name in names:
 p=ROOT/f'build/system/libs/native/{name}.LBS'
 rows=[r.split() for r in subprocess.check_output(['nm','-D','--defined-only',str(p)],text=True).splitlines() if r.strip()]
 exported=[r[-1] for r in rows if r[-1] not in ['bk_lbs_info','bk_lbs_live_resources','bk_lbs_dependency_abis',f'bk_{name.lower()}_abi_1']]
 lines += [f'## {name}.LBS','',f'{len(rows)} exportaciones totales; {p.stat().st_size} bytes en disco.','', '```text']
 lines+=exported;lines+=['```','']
(ROOT/'docs/native/LBS-EXPORTS.md').write_text('\n'.join(lines))
print('Wrote docs/native/LBS-EXPORTS.md from actual ELF exports')
