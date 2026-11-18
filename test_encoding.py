#!/usr/bin/env python3
"""Decode emitted SPI waveform independently and check factory wire fields."""
import pathlib
import subprocess
import sys
import tempfile

exe = str(pathlib.Path(sys.argv[1]).resolve())


def check(tmp, chips, gray, gains, control, flip, tail):
    path = pathlib.Path(tmp) / 'wave.bin'
    subprocess.run([exe, '-n', str(chips), '-g', ','.join(map(str, gray)),
                    '-a', ','.join(map(str, gains)), '-c', str(control),
                    '-f', str(flip), '-p', str(tail), '-o', str(path)], check=True)
    data = path.read_bytes()
    size = 261 + 48 * chips + 24
    assert len(data) == 2 * size <= 4096
    assert data[:size] == data[size:]
    frame = data[:size]
    assert frame[:128] == bytes(128)
    assert frame[128:133] == b'\xff\xff\xff\xff\x00'
    assert frame[-128:] == bytes(128)
    symbols = ''.join(f'{b:08b}' for b in frame[133:-128])
    bits = ''
    for i in range(0, len(symbols), 4):
        symbol = symbols[i:i + 4]
        assert symbol in ('1000', '1110')
        bits += '1' if symbol == '1110' else '0'
    chip_bits = '10010110' + ''.join(f'{v:016b}' for v in gray) + f'{control:08b}'
    reg_bits = '11100001' + ''.join(f'{v:06b}' for v in gains) + f'{flip}0' + f'{tail:08b}'
    assert len(reg_bits) == 48
    assert bits == chip_bits * chips + reg_bits


with tempfile.TemporaryDirectory() as tmp:
    check(tmp, 1, [0]*5, [25,25,25,63,63], 0, 0, 0xc0)
    check(tmp, 3, [0x1234,0x8001,0xabcd,0,65535], [0,1,31,32,63], 8, 1, 0x98)
    check(tmp, 32, [65535]*5, [63]*5, 255, 1, 255)
    path = str(pathlib.Path(tmp) / 'default.bin')
    subprocess.run([exe, '-o', path], check=True)
    assert len(pathlib.Path(path).read_bytes()) == 666
    default = pathlib.Path(path).read_bytes()
    subprocess.run([exe, '-t', '-o', path], check=True)
    assert pathlib.Path(path).read_bytes() == default
    for args in [['-n','0'], ['-n','33'], ['-s','1599999'], ['-s','3200001'],
                 ['-r','1'], ['-i','0'], ['-g','1,2,3,4'], ['-g','1,2,3,4,5,6'],
                 ['-g','1,2,3,4,65536'], ['-a','64,0,0,0,0'], ['-f','2']]:
        result = subprocess.run([exe, *args, '-o', path], capture_output=True)
        assert result.returncode != 0, args
    for hz in [1600000, 2000000, 3200000]:
        subprocess.run([exe, '-s', str(hz), '-o', path], check=True)
        period = 4 / hz
        assert 1.25e-6 <= period <= 2.5e-6
        assert 128 * 8 / hz >= 200e-6
        assert 200e-9 <= 8 / hz <= 20e-6
print('PASS: waveform, field order, cascade, wake-up duplication, bounds and timing')
