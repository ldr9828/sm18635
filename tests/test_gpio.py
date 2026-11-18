"""Independent SM18635 wire field and nominal positive-pulse plan checks.

The wrapped executable verifies the userspace request path, not kernel GPIO
hardware, IRQ latency or actual ndelay accuracy.
"""
import os
import pathlib
import subprocess
import sys
import tempfile

exe, mock = [str(pathlib.Path(p).resolve()) for p in sys.argv[1:3]]


def run(program, args=(), mode='', ok=True, pin=72):
    result = subprocess.run([program, *args], capture_output=True, text=True,
                            env=dict(os.environ, GPIO_MOCK_MODE=mode,
                                     GPIO_MOCK_PIN=str(pin)), timeout=3)
    assert (result.returncode == 0) == ok, (args, mode, result.stdout, result.stderr)
    return result


cases = [(1, [0]*5, [25,25,25,63,63], 0, 0, 0xc0),
         (3, [0x1234,0x8001,0xabcd,0,65535], [0,1,31,32,63], 8, 1, 0x98),
         (4, [65535]*5, [63]*5, 255, 1, 255)]
with tempfile.TemporaryDirectory(prefix='sm18635-gpio-test-') as tmp:
    dest = pathlib.Path(tmp) / 'spi-reference.bin'
    for chips, gray, gain, control, flip, reg in cases:
        args = ['-n', str(chips), '-g', ','.join(map(str, gray)), '-a',
                ','.join(map(str, gain)), '-c', str(control), '-f', str(flip),
                '-p', str(reg)]
        chip_bits = '10010110' + ''.join(f'{v:016b}' for v in gray) + f'{control:08b}'
        shared = '11100001' + ''.join(f'{v:06b}' for v in gain) + f'{flip}0{reg:08b}'
        bits = chip_bits * chips + shared
        wave = ''.join('1110' if b == '1' else '1000' for b in bits)
        run(exe, [*args, '-o', str(dest)])
        data = dest.read_bytes()
        one = 285 + 48*chips
        assert len(data) == 2*one and data[:one] == data[one:]
        assert data[:128] == bytes(128) and data[128:133] == bytes.fromhex('ffffffff00')
        assert data[one-128:one] == bytes(128)
        assert ''.join(f'{v:08b}' for v in data[133:one-128]) == wave
        result = run(mock, ['-d', '/dev/mockgpio', *args])
        pulse_line = next(x for x in result.stdout.splitlines() if x.startswith('MOCK pulses='))
        pulses = [tuple(map(int, x.split('/'))) for x in pulse_line.split('=')[1].strip(',').split(',')]
        assert pulses == [(16000,4000)] + [(1500,500) if b == '1' else (500,1500) for b in bits]
        assert sum(h+l for h,l in pulses) == 20000 + 2000*len(bits) <= 884000
    for program in [exe, mock]:
        assert 'MOCK' not in run(program, ['-h']).stdout
        assert 'MOCK' not in run(program, ['-o', str(dest)]).stdout
    for args in [['-n','0'], ['-n','5'], ['-g','1,2,3,4'], ['-g','1,2,3,4,65536'],
                 ['-a','64,0,0,0,0'], ['-f','2'], ['-p','256'], ['extra'], ['-b','2000000']]:
        run(exe, [*args, '-o', str(dest)], ok=False)
assert 'GPIO TX completed' in run(mock).stdout
for mode in ['open', 'abi', 'gpio-query', 'gpio-invalid', 'short', 'io', 'busy', 'eintr', 'timeout', 'close']:
    assert 'GPIO TX completed' not in run(mock, mode=mode, ok=False).stdout
for gpio in [0, 31, 32, 63, 64, 72, 95]:
    label = f'GPIO{gpio}/P{chr(ord("A") + gpio//32)}{gpio%32:02d}'
    assert label in run(mock, pin=gpio).stdout
print('PASS: MSB-first fields, 1/3/4 chips, nominal positive-pulse timing, SPI reference, '
      '28-byte ABI, runtime GPIO query, one TX-only request, validation and no retry on failure')
