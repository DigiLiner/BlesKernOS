#!/usr/bin/env python3
"""Genera la cobertura desde el dispatcher; presencia no significa correccion."""
from pathlib import Path
import json
import re
ROOT=Path(__file__).resolve().parents[3]
names={int(n):name for n,name in re.findall(r'LINUX_SYSCALL\((\d+)U, (\w+)\)',(ROOT/'kernel/linux/i386_syscalls.inc').read_text())}
mapped={int(n) for n in re.findall(r'case (\d+)U:',(ROOT/'kernel/linux/i386_translate.inc').read_text())}
critical={2,42,55,67,82,83,85,93,102,114,119,140,141,142,145,146,168,173,174,175,179,186,194,220,254,255,256,295,300,305,308,309,319,327,328,329,330,331,340,355,359,360,370,371,372,373,383,386,403,407,413,422,435,436,449}
notes={'close': 'PARTIAL|Archivos reales; cerrar consola sin redireccion no se modela como Linux.',
 'getpid': 'IMPLEMENTED|Devuelve ID de proceso.',
 'gettid': 'IMPLEMENTED|Devuelve ID de tarea.',
 'umask': 'INCORRECT|Devuelve 0022 sin guardar la mascara.',
 'getpgid': 'INCORRECT|No valida PID ni administra grupos reales.',
 'getpgrp': 'INCORRECT|Usa ID de proceso, no grupo independiente.',
 'brk': 'PARTIAL|Reserva fisica contigua de 4 MiB, no al final de la imagen.',
 'mmap': 'PARTIAL|old_mmap con offset en bytes alineado; mismos limites que mmap2.',
 'munmap': 'PARTIAL|Solo reserva completa exacta; no divide ni tolera huecos como Linux.',
 'set_thread_area': 'PARTIAL|Un descriptor TLS (GDT 6); glibc inicializa GS. No son las tres '
                    'entradas Linux.',
 'get_thread_area': 'PARTIAL|Solo descriptor GDT 6.',
 'writev': 'PARTIAL|iovec i386 validado, <=1024, escritura por fragmentos; regular/consola, sin '
           'atomicidad de pipes.',
 'getrandom': 'INCORRECT|Reutiliza RDRAND si existe; fallback historico sin entropia '
              'criptografica.',
 'statx': 'MISSING|glibc 2.39 cae a fstatat64 tras ENOSYS.',
 'rseq': 'MISSING|glibc 2.39 tolera ENOSYS en este bootstrap.',
 'clock_gettime64': 'MISSING|glibc cae a clock_gettime de 32 bits en pruebas anteriores a 2038.',
 'clock_nanosleep': 'PARTIAL|Solo relativo; traduccion historica no valida reloj; sin interrupcion '
                    'por senales.',
 'sched_yield': 'IMPLEMENTED|Usa scheduler existente; conserva registros de la tarea llamante.'}
notes.update(json.loads((ROOT/'ports/wine/versions/linux-i386-status.json').read_text()))
# sigreturn y wait4 se interceptan antes del traductor para preservar el frame.
mapped|={119,173,114}
lines=['# Cobertura Linux i386','',
'Auditoria de codigo y pruebas glibc 2.39 / Wine 11.0. Regenerar con `python3 ports/wine/scripts/audit-syscalls.py`.',
'','`IMPLEMENTED`: operacion acotada implementada. `PARTIAL`: cobertura incompleta.',
'`MISSING`: ENOSYS. `INCORRECT`: hay codigo, pero no cumple el contrato Linux.',
'`NOT YET TESTED`: sin evidencia para afirmar conformidad completa. Ninguna fila certifica Wine.',
'','Las llamadas no incluidas en la tabla de traduccion retornan -38; nunca caen en una syscall nativa del mismo numero.',
'','| N.º | Nombre | Estado | Observacion |','|---:|---|---|---|']
for nr in sorted(mapped|critical):
    name=names.get(nr,'unknown')
    status,note=notes.get(name,('PARTIAL|Traduccion presente; semantica completa NOT YET TESTED.' if nr in mapped else 'MISSING|Sin traduccion Linux i386.')).split('|',1)
    lines.append(f'| {nr} | `{name}` | {status} | {note} |')
lines+=['','Ver [HOST_STATUS.md](HOST_STATUS.md) para resultados de cada ejecutable y [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md) para los bloqueos de Wine.','']
(ROOT/'ports/wine/docs/SYSCALL_STATUS.md').write_text('\n'.join(lines))
