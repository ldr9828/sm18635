#!/usr/bin/env python3
"""Check that each invocation submits exactly one TX-only message and exits."""
import os
from pathlib import Path
import subprocess
import sys

exe = str(Path(sys.argv[1]).resolve())
mock = str(Path(sys.argv[2]).resolve())
for args, settings, code in [([], {}, 0), (['-t'], {}, 0),
                            ([], {'MOCK_SHORT_TRANSFER': '1'}, 1),
                            ([], {'MOCK_FAIL_AT': '1'}, 1)]:
    env = dict(os.environ, LD_PRELOAD=mock, MOCK_EXPECT_MESSAGES='1', **settings)
    result = subprocess.run([exe, *args], env=env, text=True,
                            capture_output=True, timeout=3)
    assert result.returncode == code, (result.stdout, result.stderr)
    if code == 0:
        assert 'Transfer completed.' in result.stdout, result.stdout
print('PASS: one TX-only message, automatic exit, short-transfer and timeout errors')
