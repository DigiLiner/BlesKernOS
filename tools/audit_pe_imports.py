#!/usr/bin/env python3
"""Inventory PE32 imports without executing the binary. No compatibility claim.

Usage: python3 tools/audit_pe_imports.py /path/to/office95 --json report.json
A directory scans EXE/DLL/OCX/DRV files recursively, including app-local DLLs.
Reports normal/delay imports, ordinal imports and non-PE/unsupported images.
"""
import argparse
import json
from pathlib import Path
import struct

class PEError(ValueError):
    pass

class PE:
    def __init__(self, data):
        self.data = data
        self.sections = []
        self.headers = 0
        if self.read(0, 2) != b'MZ':
            raise PEError('not an MZ executable')
        pe = self.u32(0x3c)
        signature = self.read(pe, 4)
        if signature != b'PE\0\0':
            raise PEError('NE/Win16 executable' if signature[:2] == b'NE' else 'not a PE executable')
        if self.u16(pe + 4) != 0x14c:
            raise PEError('not i386 (this target requires PE32 i386)')
        opt = pe + 24
        size = self.u16(pe + 20)
        if size < 96 or self.u16(opt) != 0x10b:
            raise PEError('not a valid PE32 optional header')
        self.read(opt, size)
        self.base = self.u32(opt + 28)
        self.headers = self.u32(opt + 60)
        self.directories = []
        count = min(self.u32(opt + 92), (size - 96) // 8)
        for i in range(count):
            self.directories.append((self.u32(opt + 96 + i*8), self.u32(opt + 100 + i*8)))
        for i in range(self.u16(pe + 6)):
            section = opt + size + i*40
            self.read(section, 40)
            self.sections.append((self.u32(section+12), self.u32(section+16), self.u32(section+20)))

    def read(self, offset, size):
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise PEError('truncated or out-of-range data')
        return self.data[offset:offset+size]

    def u16(self, offset):
        return struct.unpack('<H', self.read(offset, 2))[0]

    def u32(self, offset):
        return struct.unpack('<I', self.read(offset, 4))[0]

    def offset(self, rva, size=1):
        if 0 <= rva < self.headers and rva+size <= self.headers:
            self.read(rva, size)
            return rva
        for start, length, raw in self.sections:
            if start <= rva and rva+size <= start+length:
                self.read(raw+rva-start, size)
                return raw+rva-start
        raise PEError(f'RVA 0x{rva:x} is not file-backed')

    def string(self, rva):
        result = bytearray()
        for i in range(4096):
            ch = self.data[self.offset(rva+i)]
            if ch == 0:
                return result.decode('ascii', errors='backslashreplace')
            result.append(ch)
        raise PEError('unterminated import name')

    def imports(self):
        result = []
        for directory, stride, kind in [(1,20,'normal'), (13,32,'delay')]:
            if directory >= len(self.directories):
                continue
            rva, size = self.directories[directory]
            if not rva:
                continue
            terminated = False
            for pos in range(0, size - stride + 1, stride):
                off = self.offset(rva+pos, stride)
                fields = struct.unpack('<' + 'I'*(stride//4), self.read(off,stride))
                if not any(fields):
                    terminated = True
                    break
                if kind == 'normal':
                    names, dll = fields[0] or fields[4], fields[3]
                    va_mode = False
                else:
                    va_mode = not fields[0] & 1
                    dll, names = fields[1], fields[4] or fields[3]
                    if va_mode:
                        dll -= self.base
                        names -= self.base
                if not names or not dll:
                    raise PEError('missing import name/thunk table')
                symbols = []
                for i in range(len(self.data)//4):
                    value = self.u32(self.offset(names+i*4,4))
                    if not value:
                        break
                    if value & 0x80000000:
                        symbols.append({'ordinal':value & 0xffff})
                    else:
                        if va_mode:
                            value -= self.base
                        symbols.append({'name':self.string(value+2)})
                else:
                    raise PEError('unterminated import thunk table')
                result.append({'dll':self.string(dll).upper(), 'kind':kind, 'imports':symbols})
            if not terminated:
                raise PEError(f'unterminated {kind} import directory')
        return result

def audit(path):
    data = path.read_bytes()
    try:
        pe = PE(data)
        return {'path':str(path), 'image_base':hex(pe.base), 'imports':pe.imports()}
    except PEError as error:
        return {'path':str(path), 'error':str(error)}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path', type=Path)
    parser.add_argument('--json', type=Path, dest='output')
    args = parser.parse_args()
    if not args.path.exists():
        parser.error('path does not exist')
    paths = [args.path] if args.path.is_file() else sorted(
        p for p in args.path.rglob('*') if p.is_file() and p.suffix.lower() in {'.exe','.dll','.ocx','.drv'})
    report = {'note':'Imports only; availability and runtime behavior are not verified.',
              'files':[audit(path) for path in paths]}
    encoded = json.dumps(report, indent=2, ensure_ascii=False)+'\n'
    if args.output:
        args.output.write_text(encoded)
    else:
        print(encoded, end='')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
