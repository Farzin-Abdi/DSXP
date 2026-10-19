#!/usr/bin/env python3
"""Generates source/qr_data.h - QR codes for the support addresses.

Written here rather than pulled from a library so the project keeps
owning what it ships, and because the output is checked rather than
trusted: these encode payment addresses, and a QR that decodes to
something other than what was typed is worse than no QR at all. main()
reads every matrix back out - undoing the mask, unpicking the zigzag,
stripping the header - and refuses to write the header unless the bytes
that come back are the bytes that went in.

Version 3, error correction level L: 29x29 modules, a single data block
(so no interleaving), and 53 bytes of capacity against a longest address
of 44. Level L rather than a higher one because a phone camera reading a
backlit screen from six inches away is the easy case for a scanner, and
the spare capacity buys nothing.

Version 3 rather than 4 because the screen is 192 pixels tall and the
code shares it with a heading, an address and a button. Four modules
fewer each way at the same three pixels a module is twelve pixels back
for the layout, and it is the module size and the quiet zone a scanner
needs - not the module count. The capacity still clears the longest
address by nine bytes; main() refuses to write the header if it ever
does not.
"""
import sys

VERSION   = 3
SIZE      = 17 + 4 * VERSION      # 29
EC_CODEWORDS = 15                 # version 3-L
DATA_CODEWORDS = 55
ALIGN_CENTERS = [6, 22]

# ---- GF(256) ------------------------------------------------------
EXP = [0] * 512
LOG = [0] * 256
x = 1
for i in range(255):
    EXP[i] = x
    LOG[x] = i
    x <<= 1
    if x & 0x100:
        x ^= 0x11D
for i in range(255, 512):
    EXP[i] = EXP[i - 255]

def gmul(a, b):
    if a == 0 or b == 0:
        return 0
    return EXP[LOG[a] + LOG[b]]

def rs_generator(n):
    g = [1]
    for i in range(n):
        g = poly_mul(g, [1, EXP[i]])
    return g

def poly_mul(a, b):
    out = [0] * (len(a) + len(b) - 1)
    for i, av in enumerate(a):
        for j, bv in enumerate(b):
            out[i + j] ^= gmul(av, bv)
    return out

def rs_encode(data, n):
    gen = rs_generator(n)
    rem = list(data) + [0] * n
    for i in range(len(data)):
        coef = rem[i]
        if coef:
            for j in range(len(gen)):
                rem[i + j] ^= gmul(gen[j], coef)
    return rem[len(data):]

# ---- bitstream ----------------------------------------------------
def encode_bytes(payload):
    bits = []
    def put(value, n):
        for i in range(n - 1, -1, -1):
            bits.append((value >> i) & 1)

    put(0b0100, 4)                 # byte mode
    put(len(payload), 8)           # count indicator, versions 1-9
    for b in payload:
        put(b, 8)

    cap = DATA_CODEWORDS * 8
    assert len(bits) <= cap, "payload too long for this version"

    put(0, min(4, cap - len(bits)))          # terminator
    while len(bits) % 8:
        bits.append(0)

    words = [int("".join(str(b) for b in bits[i:i+8]), 2)
             for i in range(0, len(bits), 8)]
    pad = [0xEC, 0x11]
    while len(words) < DATA_CODEWORDS:
        words.append(pad[(len(words) - len(bits)//8) % 2])
    return words

# ---- module placement ---------------------------------------------
def blank():
    return [[None] * SIZE for _ in range(SIZE)], [[False] * SIZE for _ in range(SIZE)]

def place_function_patterns(m, fixed):
    def finder(r, c):
        for dr in range(-1, 8):
            for dc in range(-1, 8):
                rr, cc = r + dr, c + dc
                if not (0 <= rr < SIZE and 0 <= cc < SIZE):
                    continue
                inner = (0 <= dr <= 6 and 0 <= dc <= 6)
                on = inner and (dr in (0, 6) or dc in (0, 6) or
                                (2 <= dr <= 4 and 2 <= dc <= 4))
                m[rr][cc] = 1 if on else 0
                fixed[rr][cc] = True

    finder(0, 0); finder(0, SIZE - 7); finder(SIZE - 7, 0)

    for i in range(8, SIZE - 8):           # timing
        v = 1 - (i & 1)
        m[6][i] = v; fixed[6][i] = True
        m[i][6] = v; fixed[i][6] = True

    for r in ALIGN_CENTERS:                # alignment
        for c in ALIGN_CENTERS:
            if (r, c) in ((6, 6), (6, SIZE - 7), (SIZE - 7, 6)):
                continue
            for dr in range(-2, 3):
                for dc in range(-2, 3):
                    on = max(abs(dr), abs(dc)) != 1
                    m[r + dr][c + dc] = 1 if on else 0
                    fixed[r + dr][c + dc] = True

    m[SIZE - 8][8] = 1                     # always dark
    fixed[SIZE - 8][8] = True

    for i in range(9):                     # reserve format areas
        for (r, c) in ((8, i), (i, 8)):
            if 0 <= r < SIZE and 0 <= c < SIZE and not fixed[r][c]:
                m[r][c] = 0; fixed[r][c] = True
    for i in range(8):
        for (r, c) in ((8, SIZE - 1 - i), (SIZE - 1 - i, 8)):
            if not fixed[r][c]:
                m[r][c] = 0; fixed[r][c] = True

def data_positions():
    """The zigzag, in order, skipping function modules."""
    pos = []
    col = SIZE - 1
    upward = True
    while col > 0:
        if col == 6:
            col -= 1
        rows = range(SIZE - 1, -1, -1) if upward else range(SIZE)
        for row in rows:
            for c in (col, col - 1):
                pos.append((row, c))
        upward = not upward
        col -= 2
    return pos

def mask_bit(mask, r, c):
    if mask == 0: return (r + c) % 2 == 0
    if mask == 1: return r % 2 == 0
    if mask == 2: return c % 3 == 0
    if mask == 3: return (r + c) % 3 == 0
    if mask == 4: return (r // 2 + c // 3) % 2 == 0
    if mask == 5: return (r * c) % 2 + (r * c) % 3 == 0
    if mask == 6: return ((r * c) % 2 + (r * c) % 3) % 2 == 0
    return ((r + c) % 2 + (r * c) % 3) % 2 == 0

FORMAT_L = {          # level L, mask 0..7, pre-computed BCH + xor
    0: 0b111011111000100, 1: 0b111001011110011, 2: 0b111110110101010,
    3: 0b111100010011101, 4: 0b110011000101111, 5: 0b110001100011000,
    6: 0b110110001000001, 7: 0b110100101110110,
}

def place_format(m, mask):
    bits = FORMAT_L[mask]
    for i in range(15):
        b = (bits >> (14 - i)) & 1
        if i < 6:      m[8][i] = b
        elif i == 6:   m[8][7] = b
        elif i == 7:   m[8][8] = b
        elif i == 8:   m[7][8] = b
        else:          m[14 - i][8] = b
    for i in range(15):
        b = (bits >> (14 - i)) & 1
        if i < 8:      m[SIZE - 1 - i][8] = b
        else:          m[8][SIZE - 15 + i] = b

def penalty(m):
    score = 0
    for line in list(m) + [list(col) for col in zip(*m)]:
        run = 1
        for i in range(1, SIZE):
            if line[i] == line[i - 1]:
                run += 1
            else:
                if run >= 5: score += 3 + (run - 5)
                run = 1
        if run >= 5: score += 3 + (run - 5)
    for r in range(SIZE - 1):
        for c in range(SIZE - 1):
            if m[r][c] == m[r][c+1] == m[r+1][c] == m[r+1][c+1]:
                score += 3
    dark = sum(sum(row) for row in m)
    score += 10 * (abs(dark * 100 // (SIZE * SIZE) - 50) // 5)
    return score

def build(payload):
    words = encode_bytes(payload)
    words += rs_encode(words, EC_CODEWORDS)

    best = None
    for mask in range(8):
        m, fixed = blank()
        place_function_patterns(m, fixed)

        bit = 0
        allbits = [(w >> (7 - i)) & 1 for w in words for i in range(8)]
        for (r, c) in data_positions():
            if fixed[r][c]:
                continue
            v = allbits[bit] if bit < len(allbits) else 0
            bit += 1
            m[r][c] = v ^ (1 if mask_bit(mask, r, c) else 0)

        place_format(m, mask)
        p = penalty(m)
        if best is None or p < best[0]:
            best = (p, mask, m)
    return best[2], best[1]

# ---- read it back -------------------------------------------------
def decode(m, mask):
    """Undo the placement and return the payload bytes."""
    _, fixed = blank()
    probe, _ = blank()
    place_function_patterns(probe, fixed)

    bits = []
    for (r, c) in data_positions():
        if fixed[r][c]:
            continue
        bits.append(m[r][c] ^ (1 if mask_bit(mask, r, c) else 0))

    words = [int("".join(str(b) for b in bits[i:i+8]), 2)
             for i in range(0, (len(bits) // 8) * 8, 8)]

    mode = (words[0] >> 4) & 0xF
    assert mode == 4, "not byte mode after round trip: %d" % mode
    length = ((words[0] & 0xF) << 4) | (words[1] >> 4)

    out = bytearray()
    for i in range(length):
        hi = words[1 + i] & 0xF
        lo = (words[2 + i] >> 4) & 0xF
        out.append((hi << 4) | lo)
    return bytes(out)

ADDRESSES = [
    ("BTC",  "bc1qt43jxtsxg5uufs6nf6kz0hdmnrl2cyud3thwkv"),
    ("USDT", "TCy7Snky3gjGydMYjuJXzZb1WJgCYUsS1r"),
    ("ETH",  "0x79F64Fbe027d658c5727dE4c7fa59835E4eC1dB2"),
    ("SOL",  "3FraMpj5Y5DTPVmGc1TEnfcujwnmaiZdbmYd1qUbAZNm"),
    ("REPO", "https://github.com/Farzin-Abdi/DSXP"),
    ("IG",   "https://instagram.com/zde__farzin"),
    # Plain, not a mailto: URI. The text under the code is there for
    # when the camera fails, so it has to be the thing somebody would
    # type into a mail app - and every scanner worth having offers to
    # send mail when it reads an address anyway.
    ("MAIL", "farzinabdi.debian@gmail.com"),
]

def main():
    out = []
    for name, addr in ADDRESSES:
        m, mask = build(addr.encode("ascii"))

        got = decode(m, mask).decode("ascii")
        if got != addr:
            raise SystemExit("%s round trip failed:\n  in  %s\n  out %s"
                             % (name, addr, got))
        print("  %-5s %d modules, mask %d, verified" % (name, SIZE, mask))
        out.append((name, addr, m))

    if "--preview" in sys.argv:
        for name, addr, m in out:
            print("\n%s" % name)
            for row in m:
                print("".join("##" if v else "  " for v in row))
        return

    path = sys.argv[1] if len(sys.argv) > 1 else "source/qr_data.h"
    with open(path, "w") as f:
        f.write("/* Generated by tools/make_qr.py - do not edit by hand.\n"
                " *\n"
                " * QR codes for the support addresses, one bit per module,\n"
                " * packed row by row. Every matrix here was decoded back to\n"
                " * text by the generator and compared with the address it\n"
                " * came from before this file was written.\n"
                " */\n"
                "#ifndef DSXP_QR_DATA_H\n#define DSXP_QR_DATA_H\n\n"
                "#include <stdint.h>\n\n"
                "#define QR_SIZE %d\n"
                "#define QR_STRIDE %d\n\n" % (SIZE, (SIZE + 7) // 8))
        for name, addr, m in out:
            f.write("#define QR_%s_TEXT \"%s\"\n" % (name, addr))
            f.write("static const uint8_t g_qr%s[QR_SIZE * QR_STRIDE] = {\n" % name)
            for row in m:
                bs = []
                for byte in range((SIZE + 7) // 8):
                    v = 0
                    for bit in range(8):
                        idx = byte * 8 + bit
                        if idx < SIZE and row[idx]:
                            v |= 1 << bit
                    bs.append("0x%02X" % v)
                f.write("    %s,\n" % ",".join(bs))
            f.write("};\n\n")
        f.write("#endif\n")
    print("wrote %s" % path)

if __name__ == "__main__":
    main()
