#!/usr/bin/env python3
"""Resolve PERF:PC bins using the exact ELF files and PERF:ELF image bases.

Example: --image build/kernel.elf@0 --image build/programs/tuxracer.o@0xe98960
Bins span 128 bytes and can intersect multiple functions; no exact-PC claim.
"""
import argparse
from collections import Counter
from pathlib import Path
import re
import struct


def symbols(spec):
    filename, base = spec.rsplit('@', 1)
    base = int(base, 0)
    data = Path(filename).read_bytes()
    if data[:6] != b'\x7fELF\x01\x01':
        raise ValueError(f'{filename}: expected little-endian ELF32')
    header = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    kind, section_offset, stride, count = header[1], header[6], header[11], header[12]
    sections = [struct.unpack_from('<10I', data, section_offset + i * stride)
                for i in range(count)]
    addresses, cursor = {}, base
    for index, section in enumerate(sections):
        if not section[2] & 2:
            continue
        alignment = section[8] or 1
        cursor = (cursor + alignment - 1) & ~(alignment - 1)
        addresses[index] = cursor if kind == 1 else base + section[3]
        cursor += section[5]
    result = []
    for section in sections:
        if section[1] != 2:
            continue
        strings_section = sections[section[6]]
        strings = data[strings_section[4]:strings_section[4] + strings_section[5]]
        for offset in range(section[4], section[4] + section[5], section[9]):
            name, value, size, info, _, index = struct.unpack_from('<IIIBBH', data, offset)
            if info & 15 != 2 or not size or index not in addresses:
                continue
            label = strings[name:strings.find(b'\0', name)].decode(errors='replace')
            address = addresses[index] + value if kind == 1 else base + value
            result.append((address, address + size, f'{Path(filename).name}:{label}'))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--image', action='append', required=True, metavar='ELF@BASE')
    parser.add_argument('--tid', type=int)
    parser.add_argument('--race', action='store_true')
    args = parser.parse_args()
    entries = [entry for spec in args.image for entry in symbols(spec)]
    log = args.log.read_bytes().replace(b'\0', b'').decode(errors='replace')
    if args.race:
        start = log.find('[VALIDATION] click inside Tux')
        if start < 0:
            raise ValueError('no race marker')
        log = log[start:]
        hearts = list(re.finditer(r'\[VALIDATION\] alive after click', log))
        if len(hearts) >= 6:
            log = log[:hearts[5].start()]
    totals = Counter()
    for tid, pc, hits in re.findall(r'\[PERF:PC\] tid=(\d+) pc=(0x[0-9a-f]+) hits=(\d+)', log):
        if args.tid is not None and int(tid) != args.tid:
            continue
        address = int(pc, 16)
        names = sorted({label for low, high, label in entries
                        if low < address + 128 and high > address})
        totals[(tid, ' | '.join(names) or pc)] += int(hits)
    print('Samples from reported top bins only; collisions omitted; a bin may span functions.')
    for (tid, name), hits in totals.most_common(25):
        print(f'{hits:6d} tid={tid:>3} {name}')


if __name__ == '__main__':
    main()
