"""Compile and run extracted production code in local C fixtures."""
from pathlib import Path
import subprocess
import tempfile


def run_c(source, flags=()):
    with tempfile.TemporaryDirectory(prefix='q1000k-pon-test-') as tmp:
        c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
        if 'void q1000k_trace(' not in source:
            source = '#define q1000k_trace(event,id,result,a,b,c,d) do { (void)(id); (void)(result); (void)(a); (void)(b); (void)(c); (void)(d); } while (0)\n#define q1000k_trace_generation(...) ((void)0)\n#define q1000k_trace_init() 0\n#define q1000k_trace_exit() ((void)0)\n' + source
        c.write_text(source)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-variable',
                        '-Wno-unused-function', '-fsanitize=undefined',
                        '-fno-sanitize-recover=all', *flags, str(c), '-o', str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True, timeout=20)
