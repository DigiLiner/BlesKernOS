"""Summarize successful native desktop submissions, not renderer FPS/vblank.

The guest buffers PIT tick histograms and prints them after closing a window.
Quantization is reported explicitly; a saturated histogram bin is a lower
bound on that individual interval. gap_max/gap_total retain exact tick totals.
"""
import math
import re


def parse_pacing(text):
    runs = []
    for match in re.finditer(r'\[VISION:PACING\] ([^\r\n]+)', text):
        line = match[1]
        fields = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', line)}
        if not {'owner', 'intervals', 'hz'} <= fields.keys() or not fields['hz']:
            continue
        gaps = [(int(ticks), int(count)) for ticks, count in re.findall(r'(\d+):(\d+),', line)]
        count = sum(n for _, n in gaps)
        if count != fields['intervals'] or not count:
            raise ValueError('Incomplete native pacing histogram')
        gaps.sort()

        def percentile(fraction):
            rank = math.ceil(count * fraction)
            total = 0
            for ticks, n in gaps:
                total += n
                if total >= rank:
                    return ticks * 1000 / fields['hz']

        total_ticks = fields.get('gap_total', sum(ticks * n for ticks, n in gaps))
        run = {
            'owner': fields['owner'], 'intervals': count,
            'resolution_ms': 1000 / fields['hz'],
            'submission_fps': count * fields['hz'] / total_ticks if total_ticks else 0,
            'p50_ms': percentile(.50), 'p95_ms': percentile(.95), 'p99_ms': percentile(.99),
            'max_ms': fields.get('gap_max', max(t for t, _ in gaps)) * 1000 / fields['hz'],
            'over_50ms': sum(n for t, n in gaps if t * 1000 > 50 * fields['hz']),
            'over_100ms': sum(n for t, n in gaps if t * 1000 > 100 * fields['hz']),
        }
        if 'gap_max_at' in fields:
            run['max_interval_index'] = fields['gap_max_at']
        # Associate native Mesa contexts with the latest START after their PID
        # announcement. A new process is used for each backend/scene run.
        prefix = text[:match.start()]
        owner = re.search(r'\[DESKBAR:SELECTOR\] pid=' + str(fields['owner']) + r'\b process=(\S+)', prefix)
        if owner:
            run['application'] = owner[1]
            start = re.search(r'\[MESADEMOS\] START (.+?) backend=(\w+)', prefix[owner.end():])
            if start:
                run.update(scene=start[1], backend=start[2])
        runs.append(run)
    return runs


def pacing_gate(runs, mesa_demos=False):
    """Reject sustained stalls; context startup is still included in results.

    At least 95% of intervals <=50ms, 99% <=83.334ms, and <1% >100ms.
    These are a floor for this VM, not a claim of locked 60Hz presentation.
    """
    measured = [r for r in runs if r['intervals'] >= 100 and (
        r.get('backend') in ('mesa35', 'gallium') if mesa_demos else
        r.get('application') == 'GLMARK2.ELF')]
    return len(measured) == (6 if mesa_demos else 1) and all(
        r['p95_ms'] <= 50.001 and r['p99_ms'] <= 83.334 and
        r['over_100ms'] / r['intervals'] < .01 for r in measured)
