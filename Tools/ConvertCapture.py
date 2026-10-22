"""Convert the engine's P6 GPU readback to PNG using only Python's standard library."""
import pathlib
import struct
import sys
import zlib

source, target = map(pathlib.Path, sys.argv[1:3])
with source.open('rb') as capture:
    if capture.readline().strip() != b'P6':
        raise ValueError('Expected P6 readback')
    width, height = map(int, capture.readline().split())
    if capture.readline().strip() != b'255':
        raise ValueError('Expected 8-bit readback')
    pixels = capture.read()
if len(pixels) != width * height * 3:
    raise ValueError('Truncated GPU readback')

def chunk(name, payload):
    return struct.pack('>I', len(payload)) + name + payload + struct.pack('>I', zlib.crc32(name + payload))

rows = b''.join(b'\0' + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
target.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
                   + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
