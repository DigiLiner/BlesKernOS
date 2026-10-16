#!/usr/bin/env python3
"""Check SVGA3D activation/presentation and optional GUI input smoke logs."""
import argparse
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--expect', choices=['gpu', 'software'], required=True)
    parser.add_argument('--input', action='store_true',
                        help='also require a click and six later scheduler heartbeats')
    parser.add_argument('--reopen', action='store_true',
                        help='also require successful close/reopen and a second presentation')
    parser.add_argument('--perf', action='store_true',
                        help='require graphics performance regression probes and no in-race readbacks')
    args = parser.parse_args()
    log = args.log.read_bytes().replace(b'\0', b'').decode('utf-8', errors='replace')
    checks = []
    if args.expect == 'gpu':
        checks.append(('pixel selftest passed', '[SVGA3D:SELFTEST] PASS phase=' in log))
        checks.append(('GPU frame accepted', '[TUXRACER] present=gpu surface=' in log))
        owners = re.findall(r'\[GUI:GPU\] viewport owner=(\d+) process=(\d+) tid=(\d+)', log)
        checks.append(('worker uses process ownership',
                       any(owner == process and process != tid for owner, process, tid in owners)))
        checks.append(('no failed pixel selftest', '[SVGA3D:SELFTEST] FAIL' not in log))
    else:
        checks.append(('pixel selftest rejected unsupported path',
                       '[SVGA3D:SELFTEST] FAIL phase=' in log))
        checks.append(('software selected', 'renderer=Mesa 3.5 software rasterizer gpu=0 direct=0' in log))
        checks.append(('CPU frame presented', '[TUXRACER] present=cpu gpu_frame=0 surface=0' in log))
        checks.append(('GPU not announced active', 'pixel validation passed' not in log))
    if args.input:
        click = re.search(r'\[VALIDATION\] click inside Tux.* ticks=(\d+)', log)
        ticks = [int(x) for x in re.findall(r'\[VALIDATION\] alive after click ticks=(\d+)', log)]
        checks.append(('click dispatched inside Tux', click is not None))
        checks.append(('six increasing heartbeats after click',
                       click is not None and len(ticks) >= 6 and
                       int(click[1]) < ticks[0] and all(a < b for a, b in zip(ticks, ticks[1:]))))
    if args.reopen:
        reopened = re.search(r'\[VALIDATION\] reopened Tux pid=([1-9]\d*)', log)
        route = 'gpu' if args.expect == 'gpu' else 'cpu'
        checks.append(('Tux reopened', reopened is not None))
        checks.append(('reopened process presented', reopened is not None and
                       f'[TUXRACER] present={route} ' in log[reopened.end():]))
    if args.perf:
        checks.append(('kernel sleep deadlines', 'kernel sleep deadlines early=0 tests=20' in log))
        for marker in ['alpha comparisons=24 PASS', 'split-batch PASS', 'decal RGB+alpha PASS',
                       'mipmaps near+far+update+off PASS']:
            checks.append((marker, marker in log))
        checks.append(('no Mesa fallback', '[MESA35:FALLBACK]' not in log))
        start = log.find('[VALIDATION] click inside Tux')
        race = log[start:] if start >= 0 else ''
        hearts = list(re.finditer(r'\[VALIDATION\] alive after click', race))
        race = race[:hearts[5].start()] if len(hearts) >= 6 else ''
        readbacks = re.findall(r'\[PERF:3D\] readback calls=(\d+)', race)
        publications = re.findall(r'\[PERF:APP\].*frames=(\d+) gpu=(\d+) cpu=0 fps=', race)
        checks.append(('race measured over six heartbeats', len(publications) >= 5))
        checks.append(('no GPU readback during race', len(readbacks) >= 5 and
                       all(int(count) == 0 for count in readbacks)))
    checks.append(('no reported validation failure', '[VALIDATION] FAIL' not in log))
    checks.append(('no reported panic', not re.search(r'\[PANIC\]|KERNEL PANIC', log)))
    checks.append(('no task exception', not re.search(r'\[TASK\] excepcion|\[ELF\] fallo process=', log)))
    for name, passed in checks:
        print(f'{"PASS" if passed else "FAIL"}: {name}')
    print('Race motion and image quality still require visual inspection.')
    return 0 if all(passed for _, passed in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
