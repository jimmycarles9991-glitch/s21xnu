#!/usr/bin/env python3
"""Generate exact RGBA PNGs for the S21IconProbe app.

No third-party Python modules: the image bytes are deliberate, so the V32
client-bytes -> upload -> texture -> display probe can be compared against
known values.
"""
import os
import struct
import sys
import zlib


def chunk(kind, data):
    return (struct.pack('>I', len(data)) + kind + data +
            struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff))


def write_png(path, width, height, pixel):
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type: none
        for x in range(width):
            r, g, b, a = pixel(x, y, width, height)
            raw.extend((r & 255, g & 255, b & 255, a & 255))
    png = (b'\x89PNG\r\n\x1a\n' +
           chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(bytes(raw), 9)) +
           chunk(b'IEND', b''))
    with open(path, 'wb') as f:
        f.write(png)


def quadrants(colours):
    # APP_COLOURS is a 2x4 grid; FOLDER_COLOURS are 2x2 grids. Both are laid
    # out row-major, so keep the test images deterministic without padding the
    # folder palettes with duplicate colours.
    if len(colours) == 4:
        def pixel(x, y, w, h):
            return colours[(y * 2 // h) * 2 + (x * 2 // w)]
    elif len(colours) == 8:
        def pixel(x, y, w, h):
            return colours[(y * 2 // h) * 4 + (x * 4 // w)]
    else:
        raise ValueError('quadrants needs 4 or 8 colours, got %d' % len(colours))
    return pixel


APP_COLOURS = [
    (255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 0, 255),
    (0, 255, 255, 255), (255, 0, 255, 255), (255, 255, 255, 255), (0, 0, 0, 255),
]

FOLDER_COLOURS = [
    [(255, 0, 0, 255), (0, 0, 255, 255), (255, 255, 0, 255), (0, 255, 0, 255)],
    [(0, 255, 255, 255), (255, 0, 255, 255), (255, 128, 0, 255), (0, 0, 0, 255)],
    [(255, 255, 255, 255), (0, 128, 255, 255), (128, 0, 255, 255), (0, 255, 128, 255)],
]


def main(out):
    os.makedirs(out, exist_ok=True)
    write_png(os.path.join(out, 'AppIcon60x60@2x.png'), 120, 120, quadrants(APP_COLOURS))
    write_png(os.path.join(out, 'AppIcon60x60@3x.png'), 180, 180, quadrants(APP_COLOURS))
    write_png(os.path.join(out, 'AppIcon76x76@2x.png'), 152, 152, quadrants(APP_COLOURS))
    write_png(os.path.join(out, 'probe180.png'), 180, 180, quadrants(APP_COLOURS))
    for i in range(9):
        name = 'folder%d.png' % (i + 1)
        colours = FOLDER_COLOURS[i % len(FOLDER_COLOURS)]
        write_png(os.path.join(out, name), 180, 180, quadrants(colours))

    # The small surfaces CoreAnimation created in the V31 log.
    write_png(os.path.join(out, 'clock28x65.png'), 28, 65,
              lambda x, y, w, h: (255, 0, 0, 255) if y < h // 2 else (0, 0, 255, 255))
    write_png(os.path.join(out, 'clock28x95.png'), 28, 95,
              lambda x, y, w, h: (0, 255, 0, 255) if y < h // 2 else (255, 255, 0, 255))
    write_png(os.path.join(out, 'clock28x28.png'), 28, 28,
              lambda x, y, w, h: (0, 0, 255, 255) if 6 <= x < 22 and 6 <= y < 22 else (255, 255, 255, 255))
    write_png(os.path.join(out, 'clock6x85.png'), 6, 85,
              lambda x, y, w, h: (255, 0, 0, 255) if y < h // 2 else (0, 255, 255, 255))
    write_png(os.path.join(out, 'clock6x6.png'), 6, 6,
              lambda x, y, w, h: (255, 255, 0, 255))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: make_icons.py OUTDIR')
    main(sys.argv[1])
