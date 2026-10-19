#!/usr/bin/env python3
"""Checks that the icon in the ROM is the icon in gfx/.

This exists because for a long time it was not. The Makefile picked the
icon up with $(wildcard $(CURDIR)/gfx/icon.bmp), and the rule that uses
it runs in a sub-make started with -C build, where $(CURDIR) is the
build directory. The wildcard found nothing there, GAME_ICON stayed
empty, and ds_rules filled it in with calico's default drawing of a DS
console.

Every proxy for "the icon shipped" was satisfied: the file existed, the
build touched it, a banner was written, the ROM booted. The picture was
simply somebody else's. The only honest check is to take the banner back
out of the ROM and compare it to the file, which is what this does.

Usage: check_icon.py [dsxp.nds] [gfx/icon.bmp]
"""
import struct
import sys


def read_bmp(path):
    """The 32x32 4bpp BMP that make_icon.py writes, as RGB rows."""
    b = open(path, 'rb').read()
    off = struct.unpack_from('<I', b, 10)[0]
    w, h = struct.unpack_from('<ii', b, 18)
    bpp = struct.unpack_from('<H', b, 28)[0]
    if (w, abs(h), bpp) != (32, 32, 4):
        raise SystemExit('  [FAIL] %s is %dx%d %dbpp, want 32x32 4bpp'
                         % (path, w, abs(h), bpp))
    ncol = struct.unpack_from('<I', b, 46)[0] or 16
    pal = [(b[54 + i * 4 + 2], b[54 + i * 4 + 1], b[54 + i * 4])
           for i in range(ncol)]
    stride = ((w * bpp + 31) // 32) * 4
    rows = []
    for y in range(abs(h)):
        r = b[off + y * stride:off + y * stride + stride]
        rows.append([pal[(r[x // 2] >> 4) if x % 2 == 0 else (r[x // 2] & 15)]
                     for x in range(w)])
    if h > 0:                       # positive height means bottom-up
        rows.reverse()
    return rows


def read_banner(path):
    """The banner baked into the ROM, as RGB rows.

    16 tiles of 8x8 in a 4x4 grid, 4bpp with the low nibble leftmost,
    and a BGR555 palette whose entry 0 is the transparent one."""
    n = open(path, 'rb').read()
    base = struct.unpack_from('<I', n, 0x68)[0]
    if base == 0 or base + 0x240 > len(n):
        raise SystemExit('  [FAIL] %s has no banner' % path)
    tiles = n[base + 0x20:base + 0x220]
    pal_raw = n[base + 0x220:base + 0x240]
    up = lambda c: (c << 3) | (c >> 2)      # 5 bits back out to 8
    pal = []
    for i in range(16):
        v = struct.unpack_from('<H', pal_raw, i * 2)[0]
        pal.append((up(v & 31), up((v >> 5) & 31), up((v >> 10) & 31)))
    img = [[(0, 0, 0)] * 32 for _ in range(32)]
    for t in range(16):
        tx, ty = (t % 4) * 8, (t // 4) * 8
        for i in range(32):
            byte = tiles[t * 32 + i]
            x, y = (i * 2) % 8, i // 4
            img[ty + y][tx + x] = pal[byte & 15]
            img[ty + y][tx + x + 1] = pal[byte >> 4]
    return img


def main():
    nds = sys.argv[1] if len(sys.argv) > 1 else 'dsxp.nds'
    bmp = sys.argv[2] if len(sys.argv) > 2 else 'gfx/icon.bmp'
    try:
        want = read_bmp(bmp)
        got = read_banner(nds)
    except FileNotFoundError as e:
        print('  [SKIP] no %s to check' % e.filename)
        return 0

    # The palette goes through BGR555 on the way in, so a channel can come
    # back up to 7/255 light. Anything further apart is a different colour,
    # and index 0 is transparent in the ROM whatever the file says it is.
    opaque = sum(1 for y in range(32) for x in range(32) if want[y][x] != want[0][0])
    bad = [(x, y) for y in range(32) for x in range(32)
           if want[y][x] != want[0][0]
           and max(abs(a - b) for a, b in zip(want[y][x], got[y][x])) > 8]

    if bad:
        print('  [FAIL] the ROM banner is not %s: %d of %d opaque pixels '
              'differ (first at %d,%d)' % (bmp, len(bad), opaque, *bad[0]),
              file=sys.stderr)
        return 1

    print('  [PASS] the ROM banner is gfx/icon.bmp (%d opaque pixels match)'
          % opaque)
    return 0


if __name__ == '__main__':
    sys.exit(main())
