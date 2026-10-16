#!/usr/bin/env python3
"""Check the SDK's actual FSQRT implementation against the host math library."""
import ctypes
import math
from pathlib import Path
import random
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parent.parent
    source = (root / 'sdk/lib/posix_math.c').read_text()
    body = source[source.index('double sqrt(double x)'):source.index('static double wrap')]
    body = body.replace('double sqrt(', 'double tested_sqrt(')
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)
        (path / 'sqrt.c').write_text('#include <math.h>\n#include <errno.h>\n' + body)
        subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-fno-builtin',
                        str(path / 'sqrt.c'), '-o', str(path / 'sqrt.so')], check=True)
        library = ctypes.CDLL(str(path / 'sqrt.so'), use_errno=True)
        sqrt = library.tested_sqrt
        sqrt.argtypes = [ctypes.c_double]
        sqrt.restype = ctypes.c_double
        values = [0., -0., 1., 2., 4., float('inf'), float('nan'),
                  5e-324, 1.7976931348623157e308]
        randomizer = random.Random(17)
        values += [math.ldexp(randomizer.uniform(0.5, 1), randomizer.randint(-1073, 1024))
                   for _ in range(20000)]
        for value in values:
            got, expected = sqrt(value), math.sqrt(value)
            assert ((math.isnan(expected) and math.isnan(got)) or got == expected or
                    abs(got - expected) <= math.ulp(expected)), (value, got, expected)
        assert math.copysign(1, sqrt(-0.)) == -1
        ctypes.set_errno(0)
        import errno
        assert math.isnan(sqrt(-1.)) and ctypes.get_errno() == errno.EDOM
    print('PASS: FSQRT 20009 cases <=1 ULP; signed zero, subnormal, infinity, NaN, EDOM')


if __name__ == '__main__':
    main()
