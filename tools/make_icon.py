#!/usr/bin/env python3
"""Generates gfx/icon.bmp - the 32x32 16-colour icon ndstool bakes into the
ROM header and the DS menu shows on the cartridge tile.

Drawn here rather than committed as an opaque binary so it can be edited,
and so nothing in this project ships artwork it did not draw itself.
Palette index 0 is what the DS treats as transparent.
"""
import struct
import sys

W = H = 32

PALETTE = [
    (0x00, 0x00, 0x00),   #  0 transparent - the menu's own ground shows through
    (0x05, 0x0E, 0x22),   #  1 outline, darkest
    (0x0A, 0x24, 0x6A),   #  2 shell, shadow
    (0x15, 0x50, 0xC4),   #  3 shell, body
    (0x4C, 0x8B, 0xE8),   #  4 shell, lit edge
    (0x9C, 0xC8, 0xFF),   #  5 shell, highlight
    (0xB8, 0xC6, 0xDC),   #  6 label, shaded
    (0xEC, 0xF2, 0xFA),   #  7 label
    (0xFF, 0xFF, 0xFF),   #  8 label, highlight
    (0x08, 0x14, 0x2C),   #  9 window, inside
    (0x2E, 0x6A, 0xD0),   # 10 hub, rim
    (0xC8, 0xDC, 0xFF),   # 11 hub, centre
    (0x1B, 0x3A, 0x7A),   # 12 tape, between the spools
] + [(0, 0, 0)] * 3


def make_pixels():
    """A cassette, which is what this player already draws on its own top
    screen - so the tile in the menu and the thing on the console are the
    same object rather than a media-player glyph standing in for it.

    Three things decide whether this reads at thirty-two pixels.

    It has to be big. A first pass gave the shell a cassette's own
    proportions, about sixteen by nine, and it looked like a sticker in
    the middle of the tile with half the height unused. The shell is
    three quarters of the square now and runs edge to edge, which costs
    some accuracy in the proportions and buys the only thing that
    matters at this size.

    The spools have to be discs. Drawn as a cross of single pixels they
    read as two plus signs, which is what the first pass did: a box with
    two plus signs in it is not a cassette. Each is a five-pixel disc
    with a rim, a lit face and a dark hole, which is as close to a spool
    as five pixels gets.

    And the window has to be worth looking at. The label is the larger
    area but it is a blank rectangle; the window is where the two hubs
    and the tape between them are, so it gets the room rather than the
    label taking it.
    """
    px = [[0] * W for _ in range(H)]

    x0, x1 = 0, 31
    y0, y1 = 4, 27                      # 32 x 24 - three quarters of the tile

    def corner(x, y):
        """True inside the clipped corners, so the shell is not a blunt
        rectangle. Two pixels, which at this size is the difference
        between a moulded shell and a brick."""
        for cx, cy in ((x0, y0), (x1, y0), (x0, y1), (x1, y1)):
            if abs(x - cx) + abs(y - cy) <= 2:
                return True
        return False

    # The shell: a lit top edge and a shadowed foot, so it has a
    # direction of light rather than being one flat colour.
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if corner(x, y):
                continue
            if y == y0 or y == y1 or x == x0 or x == x1:
                px[y][x] = 1
            elif y == y0 + 1:
                px[y][x] = 5 if x0 + 1 < x < x1 - 1 else 4
            elif y == y0 + 2:
                px[y][x] = 4
            elif y == y1 - 1:
                px[y][x] = 2
            else:
                px[y][x] = 3

    # The label: the one large light area, which is what carries the
    # shape at a glance against a dark menu.
    lx0, lx1 = 3, 28
    ly0, ly1 = y0 + 4, y0 + 11
    for y in range(ly0, ly1 + 1):
        for x in range(lx0, lx1 + 1):
            px[y][x] = 7
    for x in range(lx0, lx1 + 1):
        px[ly0][x] = 8                  # highlight along the top
        px[ly1][x] = 6                  # and a shaded foot

    # Two ruled lines, which is what a cassette label has and what keeps
    # it from reading as a plain white bar.
    for x in range(lx0 + 3, lx1 - 2):
        px[ly0 + 3][x] = 6
        px[ly0 + 5][x] = 6

    # The window, and the spools in it. Everything above could be a box;
    # a box with two spools in it is a cassette.
    wx0, wx1 = 5, 26
    wy0, wy1 = ly1 + 2, y1 - 2
    for y in range(wy0, wy1 + 1):
        for x in range(wx0, wx1 + 1):
            px[y][x] = 9

    hub_y = (wy0 + wy1) // 2
    for x in range(wx0 + 1, wx1):       # the tape, spanning between them
        px[hub_y][x] = 12

    for hub_x in (10, 21):
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                if abs(dx) == 2 and abs(dy) == 2:
                    continue            # a disc, not a square
                px[hub_y + dy][hub_x + dx] = 10
        # A flat lit face, not a rounded one. Rounding it to match the
        # rim leaves a one-pixel ring with a hole in it, and at five
        # pixels that reads as a plus sign rather than a spool - which
        # is worse than the slight squareness it was meant to fix.
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                px[hub_y + dy][hub_x + dx] = 11
        px[hub_y][hub_x] = 9            # the hole through the middle

    return px


def write_bmp(path, px):
    row_bytes = W // 2                      # 4bpp, already 4-byte aligned
    img = bytearray()
    for y in range(H - 1, -1, -1):          # BMP rows run bottom-up
        row = px[y]
        for x in range(0, W, 2):
            img.append((row[x] << 4) | row[x + 1])

    palette = b"".join(struct.pack("<BBBB", b, g, r, 0) for (r, g, b) in PALETTE)

    dib = struct.pack("<IiiHHIIiiII", 40, W, H, 1, 4, 0, len(img), 2835, 2835, 16, 16)
    offset = 14 + len(dib) + len(palette)
    header = b"BM" + struct.pack("<IHHI", offset + len(img), 0, 0, offset)

    with open(path, "wb") as f:
        f.write(header + dib + palette + bytes(img))

    print(f"wrote {path} ({W}x{H}, 16 colours, {offset + len(img)} bytes)")


def preview(px):
    ramp = " .:-=+*#%@ABCDEF"
    for row in px:
        print("".join(ramp[v] for v in row))


if __name__ == "__main__":
    pixels = make_pixels()
    if "--preview" in sys.argv:
        preview(pixels)
    out = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "gfx/icon.bmp"
    write_bmp(out, pixels)
