#!/usr/bin/env python3
"""Check real i386 API dispatch for cdecl, struct returns and ret N functions."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
assembly = (root / 'kernel/arch/x86/api_call.asm').read_text()
# The host harness runs in Ring 3: use its flat stack selector instead of 0x10.
assembly = assembly.replace('mov ax, KERNEL_DATA_SELECTOR', 'mov ax, ss')
source = r'''
typedef unsigned int u32;
typedef unsigned long long u64;
extern u64 elf_api_call_raw(u32, const u32 *, u32 *);
struct pair { u32 a, b; };
static __attribute__((noinline)) u32 add(u32 a, u32 b) { return a + b; }
static __attribute__((noinline)) struct pair make_pair(u32 a, u32 b) {
    struct pair result = {a, b}; return result;
}
static __attribute__((noinline,stdcall)) u32 std_add(u32 a, u32 b) { return a + b; }
void _start(void) {
    u32 args[16] = {7, 11}, cleanup = 99, status = 0;
    struct pair result = {0, 0};
    if ((u32)elf_api_call_raw((u32)add, args, &cleanup) != 18 || cleanup != 0) status = 1;
    args[0] = (u32)&result; args[1] = 123; args[2] = 456;
    elf_api_call_raw((u32)make_pair, args, &cleanup);
    if (cleanup != 4 || result.a != 123 || result.b != 456) status = 2;
    args[0] = 20; args[1] = 22;
    if ((u32)elf_api_call_raw((u32)std_add, args, &cleanup) != 42 || cleanup != 8) status = 3;
    __asm__ volatile("int $0x80" : : "a"(1), "b"(status) : "memory");
    __builtin_unreachable();
}
'''
with tempfile.TemporaryDirectory(prefix='bles-native-abi-') as temporary:
    path = Path(temporary)
    (path / 'api.asm').write_text(assembly)
    (path / 'test.c').write_text(source)
    subprocess.run(['nasm', '-f', 'elf32', str(path / 'api.asm'), '-o', str(path / 'api.o')], check=True)
    subprocess.run(['gcc', '-m32', '-O2', '-fno-builtin', '-fno-pie', '-no-pie', '-fno-stack-protector', '-nostdlib', str(path / 'test.c'), str(path / 'api.o'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS actual i386 API wrapper: cdecl, hidden struct return pointer and bounded ret N')
