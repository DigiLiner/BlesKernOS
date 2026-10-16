#!/usr/bin/env python3
"""Summarize perfmon publications; race selects the scripted click/heartbeat window."""
import argparse
import json
from pathlib import Path
import re


def summarize(path, phase):
    text = path.read_bytes().decode('utf-8', 'replace').replace('\x00', '')
    if phase == 'race':
        start = text.find('[VALIDATION] click inside Tux')
        if start < 0:
            raise ValueError(f'{path}: missing click marker')
        text = text[start:]
        hearts = list(re.finditer(r'alive after clic(?:k)?', text))
        if len(hearts) < 6:
            raise ValueError(f'{path}: need six post-click heartbeats')
        text = text[:hearts[5].start()]
    rows = re.findall(r'\[PERF:APP\] pid=(\d+) frames=(\d+) gpu=(\d+) cpu=(\d+) fps=([\d.]+) interval_ms=(\d+)', text)
    groups = {}
    for pid, frames, gpu, cpu, fps, elapsed in rows:
        group = groups.setdefault(pid, dict(samples=0, frames=0, gpu=0, cpu=0, interval_ms=0, fps_min=None, fps_max=0))
        group['samples'] += 1
        for key, value in [('frames', frames), ('gpu', gpu), ('cpu', cpu), ('interval_ms', elapsed)]:
            group[key] += int(value)
        group['fps_min'] = min(group['fps_min'] if group['fps_min'] is not None else float(fps), float(fps))
        group['fps_max'] = max(group['fps_max'], float(fps))
    for group in groups.values():
        group['fps_weighted'] = round(group['frames'] * 1000 / group['interval_ms'], 3)
    return dict(log=str(path), phase=phase, processes=groups,
                note='Accepted CPU/GPU publications, not monitor scanout. Only complete APP lines; intervals without publications omitted. Race boundary intervals can overlap the click; the scripted window may include results after the finish line. Verify game phase visually.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', type=Path, nargs='+')
    parser.add_argument('--phase', choices=['all', 'race'], default='all')
    args = parser.parse_args()
    print(json.dumps([summarize(path, args.phase) for path in args.logs], indent=2))


if __name__ == '__main__':
    main()
