"""Compile and run extracted production code in local C fixtures."""
from pathlib import Path
import subprocess
import tempfile


def run_c(source, flags=()):
    with tempfile.TemporaryDirectory(prefix='q1000k-pon-test-') as tmp:
        c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
        c.write_text(source)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-variable',
                        '-Wno-unused-function', '-fsanitize=undefined',
                        '-fno-sanitize-recover=all', *flags, str(c), '-o', str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True, timeout=20)
