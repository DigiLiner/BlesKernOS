#!/usr/bin/env python3
"""Validate a scripted Virtio graphics run; do not confuse submitted frames with scanout."""
import argparse
import json
from pathlib import Path
import re
from summarize_graphics_perf import summarize


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--device', choices=['virgl', 'virtio'], default='virgl')
    parser.add_argument('--race', action='store_true')
    args = parser.parse_args()
    text = args.log.read_bytes().decode(errors='replace').replace('\0', '')
    failures = []
    def require(condition, message):
        if not condition:
            failures.append(message)
    require('reopened Tux' in text, 'missing close/reopen')
    require(len(re.findall(r'alive after clic(?:k)?', text)) >= 6, 'missing six post-click heartbeats')
    require(not re.search(r'\[(?:VIRGL|R128):TEST\].*FAIL|timeout controlq|EXCEPTION|\[PANIC\]|\[TASK:EXC\]|\[TASK\] excepcion|\[ELF\] fallo process=', text),
            'pixel/transport/kernel failure')
    qemu = args.log.parent / 'qemu.log'
    if qemu.exists():
        require(not re.search(r'Illegal|illegal|context.*error|decode.*error|GL_INVALID',
                              qemu.read_text(errors='replace')), 'host renderer error')
    if args.device == 'virgl':
        for marker in ['alpha comparisons=24 PASS', 'DECAL alpha preserved PASS',
                       'native mip selection and disabled mip PASS',
                       'mip update + isolated base subimage PASS',
                       'premultiplied opacity + clip + fractional Z clear PASS',
                       'interleaved targets + command/VBO rollover PASS',
                       'VBO lifetime submissions=64 bad_tiles=0 PASS',
                       'renderer=virtio_virgl gpu=1', 'present=gpu']:
            require(marker in text, 'missing ' + marker)
        require('[MESA35:FALLBACK]' not in text, 'Mesa fallback')
    else:
        require('VirGL GFX3D/TinyGL: DRAW_VBO' not in text, '3D unexpectedly advertised without VirGL')
        require('renderer=' in text and 'gpu=0 direct=0' in text, 'software renderer not confirmed')
    report = {'log': str(args.log), 'device': args.device, 'failures': failures}
    if args.race:
        try:
            result = summarize(args.log, 'race')
            report['performance'] = result
            require(bool(result['processes']), 'no complete APP samples during race')
            for row in result['processes'].values():
                require(row['cpu'] == 0 if args.device == 'virgl' else row['gpu'] == 0,
                        'unexpected publication path')
            start = text.find('click inside Tux')
            race = text[start:]
            hearts = list(re.finditer(r'alive after clic(?:k)?', race))
            if len(hearts) >= 6:
                race = race[:hearts[5].start()]
            if args.device == 'virgl':
                require(not re.search(r'\[PERF:3D\] readback calls=[1-9]', race), 'GPU readback during race')
        except ValueError as exc:
            failures.append(str(exc))
    report['pass'] = not failures
    print(json.dumps(report, indent=2))
    raise SystemExit(bool(failures))


if __name__ == '__main__':
    main()
