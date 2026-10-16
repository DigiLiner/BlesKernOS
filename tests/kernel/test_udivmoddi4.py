#!/usr/bin/env python3
"""Compare the production i386 division helpers with Python integer arithmetic."""
from pathlib import Path
import random,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/libc/gcc_runtime.c').read_text()
s=s[s.index('uint64_t __udivdi3('):s.index('int64_t __divdi3(')]
r=random.Random(20261014)
cases=[(a,b) for a in [0,1,2**32-1,2**32,2**63,2**64-1,1700000000123456789] for b in [1,2,300,1000000000,2**32,2**63,2**64-1]]
cases += [(r.getrandbits(64),r.getrandbits(64) or 1) for _ in range(300)]
code='#include <stdint.h>\n#include <assert.h>\n#include <stdio.h>\n'+s+'\nint main(void){uint64_t rem;\n'
for a,b in cases:
 q,m=divmod(a,b)
 code+=f'assert(__udivmoddi4({a}ULL,{b}ULL,&rem)=={q}ULL && rem=={m}ULL);\n'
code+='assert(__udivmoddi4(123,10,0)==12);puts("PASS i386 udivmoddi4: 349 Python reference cases, high-bit divisors, time conversions and null remainder");}\n'
with tempfile.TemporaryDirectory(prefix='bk-divmod-') as t:
 p=Path(t);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-m32','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
