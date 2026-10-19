#!/usr/bin/env python3
"""Renders the cassette shell and bakes it as an indexed picture.

Drawn flat, the way a cassette is drawn on a sleeve rather than the way
one photographs: a black shell, one big printed label, a grey window
band, white spool hubs, a paper strip to write the title on. No grain,
no specular, no gradients - at 222 pixels across, a drawn object reads
as an object and a rendered one reads as a rendered one.

Rendered at four times the size and box-filtered down, so every curve
has true coverage at its edge and nothing is a staircase.

The label is the theme's colour, and that is why this file is more than
a picture. The label's pixels are not baked as colours at all: they are
baked as positions on three ramps - shell to label, label to white,
label to window grey - and the player fills those palette entries in
from whatever the selected theme's accent is. Change the theme and the
label changes with it, at no cost beyond twenty-eight palette writes.

Index 0 is nothing at all: the ground outside the rounded corners, and
the two portholes the spools turn behind. Both are drawn live.
"""
from PIL import Image, ImageDraw
import math, sys

# Sized to the screen it is the only thing on.
#
# Nothing else is drawn on this card now, so the shell takes the width
# the frame leaves it and the height follows from the proportions of the
# real object - 100.4 by 63.8 millimetres, which is wider than a DS
# screen and is what decides that width, not height, is the limit here.
W, H, S = 240, 152, 4

# ---- geometry -------------------------------------------------------
PANEL  = (13, 20, 226, 132)     # the printed label
HDR    = (13, 20, 226, 34)      # the band across the top of it
CY     = 66                     # the spool centres
SPX    = (71, 169)
WINR   = 25
BAND   = (SPX[0] - 17, CY - 17, SPX[1] + 17, CY + 17)
STRIP  = (26, 102, 219, 126)    # the paper you write the title on
BADGE  = (30, 105, 50, 122)     # the side letter, sitting on the paper
FLAP   = (48, 1, 191, 15)
SCREWS = ((11, 11), (229, 11), (11, 140), (229, 140))

# ---- colours --------------------------------------------------------
SHELL   = (22, 22, 25)          # the plastic
RIM     = (58, 58, 63)          # so it does not merge with a dark ground
FLAPC   = (62, 62, 67)
FLAPDOT = (30, 30, 33)
SCREWC  = (112, 112, 118)
BANDC   = (24, 24, 28)          # the window the spools turn behind
TAPEC   = (122, 114, 106)       # the pad that holds the tape to the head
TAPELIT = (158, 150, 142)       # its felt
RIBBON  = (94, 76, 56)          # and the tape itself, crossing behind it
RIBBON2 = (128, 104, 76)        # its lit edge
SCREWLIT= (156, 156, 162)
FLASH   = ((226, 87, 76), (241, 199, 66), (79, 195, 232))
STEPC   = (38, 38, 43)          # the moulded step above the openings
FLAPLIT = (88, 88, 94)
PAPER   = (247, 245, 238)
RULE    = (170, 180, 198)
INK     = (44, 44, 48)
BADGEC  = (241, 199, 66)
OPENING = (46, 46, 50)

# ---- the three ramps the theme fills in -----------------------------
#
# Twelve steps from the shell to the label, eight from the label to the
# paper, eight from the label to the window. Twelve on the first because
# that edge is the longest one in the picture - the whole outline of the
# label - and it is the one a short ramp would band.
R_SHELL, N_SHELL = 1, 12
R_PAPER, N_PAPER = 13, 8
R_BAND,  N_BAND  = 21, 8
R_HDR,   N_HDR   = 29, 6
RESERVED = 34
NPAL     = 88                   # room for the printing to have its own tones

# Printing, in the label's own colour.
#
# The label is not baked as a colour but as coverage on the shell-to-
# accent ramp, so a lower coverage is simply a darker accent - and that
# is what the printing on it is. Nothing here needs a colour of its own,
# and nothing here can clash with a theme: a rule at 190 is three
# quarters of whatever the accent turns out to be, on every one of them.
PRINT, DEEP = 190, 148

def L(mode, fill=0):
    return Image.new(mode, (W * S, H * S), fill)

def draw(im):
    return ImageDraw.Draw(im)

def box(d, r, x0, y0, x1, y1, fill):
    d.rounded_rectangle([x0 * S, y0 * S, (x1 + 1) * S - 1, (y1 + 1) * S - 1],
                        radius=r * S, fill=fill)

def rect(d, x0, y0, x1, y1, fill):
    d.rectangle([x0 * S, y0 * S, (x1 + 1) * S - 1, (y1 + 1) * S - 1], fill=fill)

def dot(d, cx, cy, r, fill):
    d.ellipse([(cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S], fill=fill)

# ---- layer: the shell, and everything printed on it -----------------
shell = L("RGB")
sd = draw(shell)
shellA = L("L")
sa = draw(shellA)

for dd, fill in ((sd, RIM), (sa, 255)):
    box(dd, 7, 0, 0, W - 1, H - 1, fill)
box(sd, 7, 1, 1, W - 2, H - 2, SHELL)

# the hinged cover across the top: a lit top edge, two pins, and the
# notches a thumb lifts it by
box(sd, 2, *FLAP, FLAPC)
sd.rectangle([(FLAP[0] + 3) * S, 2 * S, (FLAP[2] - 3) * S, 2 * S + S // 2],
             fill=FLAPLIT)
for x in (FLAP[0] + 15, FLAP[2] - 15):
    dot(sd, x, 8, 1.7, FLAPDOT)
for x in (FLAP[0] + 7, FLAP[2] - 9):
    box(sd, 1, x, 4, x + 2, 12, FLAPDOT)

# the pockets a write-protect tab breaks out of
for x0 in (8, W - 23):
    box(sd, 1, x0, 0, x0 + 14, 5, FLAPDOT)

# the openings along the bottom edge: capstans outside, heads in the
# middle, which is the arrangement a transport actually needs
for (x0, x1, y0) in ((44, 53, 139), (64, 90, 138), (99, 141, 136),
                     (150, 176, 138), (187, 196, 139)):
    box(sd, 1, x0, y0, x1, 147, OPENING)

# the moulded step the openings are sunk into, and the two holes the
# deck's alignment pins drop through
sd.rectangle([11 * S, 134 * S, (W - 12) * S, 134 * S + S // 2], fill=STEPC)
for cx in (32, W - 33):
    dot(sd, cx, 141, 2.4, FLAPDOT)
    dot(sd, cx, 141, 1.4, SHELL)

for (cx, cy) in SCREWS:
    dot(sd, cx, cy, 4.0, FLAPDOT)
    dot(sd, cx, cy, 3.2, SCREWC)
    sd.rectangle([(cx - 2.2) * S, (cy - 0.5) * S, (cx + 2.2) * S, (cy + 0.5) * S],
                 fill=FLAPDOT)
    sd.rectangle([(cx - 0.5) * S, (cy - 2.2) * S, (cx + 0.5) * S, (cy + 2.2) * S],
                 fill=FLAPDOT)
    dot(sd, cx - 1.2, cy - 1.2, 0.9, SCREWLIT)

# ---- layer: the label ----------------------------------------------
panelA = L("L")
pnl = draw(panelA)
box(pnl, 3, *PANEL, 255)

def ticks(d, x, y, widths, h, fill, gap=3):
    """A row of marks that reads as small print and is not any language."""
    for w in widths:
        d.rectangle([x * S, y * S, (x + w) * S - 1, (y + h) * S - 1], fill=fill)
        x += w + gap
    return x

# the keyline a printed inlay has, below the header band
pnl.rounded_rectangle([16 * S, 43 * S, 224 * S - 1, 129 * S - 1],
                      radius=2 * S, outline=PRINT, width=S)

# the shadow the paper strip casts, so it reads as stuck on rather than
# printed in
rect(pnl, 29, 128, 221, 128, DEEP)

# ---- layer: the header band -----------------------------------------
#
# Its own ramp, because it is the one part of the label that cannot be
# a fixed darkening of the accent. A quarter of the themes have an
# accent darker than the window, and on those a darker band is no band
# at all - it has to go the other way and print light. Which way is
# chosen at run time, from the accent, and only the far end of this ramp
# changes; the shape is baked once.
hdrA = L("L")
hd_ = draw(hdrA)
box(hd_, 3, *HDR, 255)
rect(hd_, HDR[0], HDR[3] - 6, HDR[2], HDR[3], 255)

# and the boxed type mark at the other end of the label, in the same ink
box(hd_, 1, 192, 92, 217, 100, 255)

# the small print along the bottom of the label
ticks(pnl, 22, 94, (10, 5, 8, 4, 11), 2, PRINT)

ticks(pnl, 22, 99, (7, 4, 9, 5), 1, DEEP)

# and the direction arrows, one each side of the window: which way the
# tape runs, which is the one thing a label says that this card means
for sgn in (1, -1):
    ax = 33 if sgn > 0 else W - 34
    pnl.rectangle([min(ax, ax - sgn * 11) * S, (CY - 1) * S,
                   max(ax, ax - sgn * 11) * S, (CY + 1) * S - 1], fill=PRINT)
    pnl.polygon([((ax + sgn * 7) * S, CY * S + S // 2),
                 (ax * S, (CY - 5) * S),
                 (ax * S, (CY + 6) * S - 1)], fill=PRINT)

# ---- layer: the window band ----------------------------------------
#
# A bar between two circles, not a stadium: the spools stand proud of
# the band top and bottom, the way they do on a printed sleeve. Its
# ends run far enough under the circles that its own corners never show
# - a bar that stopped short of them put a notch either side of every
# spool. The
# circles are in the mask so their edge is antialiased against the
# label - the hole punched out of them below is only ever the part the
# player is about to draw over.
bandA = L("L")
bd = draw(bandA)
box(bd, 6, *BAND, 255)
for cx in SPX:
    dot(bd, cx, CY, WINR, 255)

# what is inside the window: the dark of the shell, and the tape itself
# crossing between the two hubs
bandRGB = L("RGB")
gd = draw(bandRGB)
box(gd, 6, *BAND, BANDC)
for cx in SPX:
    dot(gd, cx, CY, WINR, BANDC)
# The tape runs hub to hub behind the pressure pad, and the pad's felt
# is the pale block in the middle. The ribbon is only visible in the two
# gaps either side of the pad - the spools cover the rest of it - which
# is exactly where a real one shows.
MIDX = (SPX[0] + SPX[1]) // 2
gd.rectangle([SPX[0] * S, (CY - 4) * S, (SPX[1] + 1) * S - 1, (CY + 5) * S - 1],
             fill=RIBBON)
# and a lit edge along it, so the two gaps either side of the pad read as
# a ribbon with a thickness rather than as a bar
gd.rectangle([SPX[0] * S, (CY - 4) * S, (SPX[1] + 1) * S - 1, (CY - 4) * S + S // 2],
             fill=RIBBON2)
box(gd, 2, MIDX - 16, CY - 16, MIDX + 16, CY + 16, BANDC)
box(gd, 2, MIDX - 15, CY - 15, MIDX + 15, CY + 15, TAPEC)
box(gd, 1, MIDX - 10, CY - 10, MIDX + 10, CY + 10, TAPELIT)


# ---- layer: the paper strip, and what is printed on it --------------
paper = L("RGB")
pd = draw(paper)
paperA = L("L")
box(draw(paperA), 1, *STRIP, 255)
box(pd, 1, *STRIP, PAPER)

# the colour flash at the far end of the header band, keylined so it
# reads whichever way that band prints
rect(pd, 189, 22, 218, 32, INK)
rect(draw(paperA), 189, 22, 218, 32, 255)
for i, c in enumerate(FLASH):
    rect(pd, 190 + i * 10, 23, 197 + i * 10, 31, c)

# a border, the way a stick-on inlay is die-cut
pd.rounded_rectangle([(STRIP[0] + 2) * S, (STRIP[1] + 2) * S,
                      (STRIP[2] - 1) * S, (STRIP[3] - 1) * S],
                     radius=1 * S, outline=RULE, width=S // 2 or 1)

# two ruled lines, the second one shorter the way an index line is
pd.rectangle([(STRIP[0] + 4) * S, 118 * S, (STRIP[2] - 4) * S, 118 * S + S // 2],
             fill=RULE)
pd.rectangle([(STRIP[0] + 4) * S, 122 * S, (STRIP[2] - 43) * S, 122 * S + S // 2],
             fill=RULE)

# the rule that separates the side letter from what you write
pd.rectangle([54 * S, 106 * S, 54 * S + S // 2, 121 * S], fill=RULE)

# the side letter, in its own box on the paper
box(pd, 2, BADGE[0] - 1, BADGE[1] - 1, BADGE[2] + 1, BADGE[3] + 1, INK)
box(pd, 2, *BADGE, BADGEC)
ax0, ay0, ax1, ay1 = BADGE
acx = (ax0 + ax1) / 2.0
pd.polygon([(acx * S, (ay0 + 3) * S),
            ((acx - 3.6) * S, (ay1 - 3) * S),
            ((acx - 1.9) * S, (ay1 - 3) * S),
            (acx * S, (ay0 + 6.4) * S)], fill=INK)
pd.polygon([(acx * S, (ay0 + 3) * S),
            ((acx + 3.6) * S, (ay1 - 3) * S),
            ((acx + 1.9) * S, (ay1 - 3) * S),
            (acx * S, (ay0 + 6.4) * S)], fill=INK)
pd.rectangle([(acx - 2.4) * S, (ay1 - 6) * S, (acx + 2.4) * S, (ay1 - 5) * S],
             fill=INK)

# ---- down to size ---------------------------------------------------
#
# A box filter, not a resample: every output pixel is the mean of the
# sixteen that made it, which is exactly the coverage its edge has.
def down(im):
    return im.resize((W, H), Image.BOX)

shell, shellA = down(shell), down(shellA)
panelA, bandA = down(panelA), down(bandA)
hdrA = down(hdrA)
bandRGB = down(bandRGB)
paper, paperA = down(paper), down(paperA)

sp, sap = shell.load(), shellA.load()
pp, pap = paper.load(), paperA.load()
plp, blp = panelA.load(), bandA.load()
hlp = hdrA.load()
brp = bandRGB.load()

# ---- compose, in index space ---------------------------------------
#
# Every pixel is one of: nothing, a place on one of the three ramps, or
# an ordinary baked colour. Only the third kind goes to the quantiser.
NOTHING, RAMP = -1, -2
kind = [[NOTHING] * W for _ in range(H)]
level = [[0] * W for _ in range(H)]
baked = [[None] * W for _ in range(H)]

def step(a, n):
    """Coverage 1..255 -> ramp step 1..n, never 0 and never past n."""
    v = (a * n + 127) // 255
    return max(1, min(n, v))

for y in range(H):
    for x in range(W):
        # The portholes are live - but only exactly as far as the
        # player paints them. It blits row dy of each window from
        # cx - isqrt(r^2 - dy^2) for twice that many pixels, so anything
        # outside that has to stay baked: punching a whole circle left a
        # ring of unpainted pixels round each spool, and the ground
        # showed through it as a dotted arc.
        dy = y - CY
        if abs(dy) <= WINR:
            half = math.isqrt(WINR * WINR - dy * dy)
            if any(cx - half <= x <= cx + half - 1 for cx in SPX):
                continue
        if sap[x, y] == 0:
            continue                                  # outside the shell

        pa, ba, wa, ha = plp[x, y], blp[x, y], pap[x, y], hlp[x, y]

        if wa:
            if wa == 255:
                kind[y][x] = 0
                baked[y][x] = pp[x, y]
            else:
                kind[y][x] = RAMP
                level[y][x] = R_PAPER - 1 + step(wa, N_PAPER)
        elif ba:
            if ba == 255:
                kind[y][x] = 0
                baked[y][x] = brp[x, y]
            else:
                kind[y][x] = RAMP
                level[y][x] = R_BAND - 1 + step(ba, N_BAND)
        elif ha:
            kind[y][x] = RAMP
            level[y][x] = R_HDR - 1 + step(ha, N_HDR)
        elif pa:
            kind[y][x] = RAMP
            level[y][x] = R_SHELL - 1 + step(pa, N_SHELL)
        else:
            kind[y][x] = 0
            baked[y][x] = sp[x, y]

# ---- quantise what is left -----------------------------------------
flat = [baked[y][x] for y in range(H) for x in range(W) if kind[y][x] == 0]
SPARE = NPAL - 1 - RESERVED
uniq = sorted(set(flat))
if len(uniq) <= SPARE:
    table = {c: i for i, c in enumerate(uniq)}
    spare = list(uniq)
else:
    tmp = Image.new("RGB", (len(flat), 1))
    tmp.putdata(flat)
    q = tmp.convert("P", palette=Image.ADAPTIVE, colors=SPARE)
    qp = q.getpalette()[: SPARE * 3]
    spare = [tuple(qp[i*3:i*3+3]) for i in range(SPARE)]
    qd = list(q.getdata())
    table = None

newpal = [(0, 0, 0)] + [(0, 0, 0)] * RESERVED + spare

idx = [0] * (W * H)
k = 0
for y in range(H):
    for x in range(W):
        if kind[y][x] == NOTHING:
            continue
        if kind[y][x] == RAMP:
            idx[y * W + x] = level[y][x]
        else:
            c = baked[y][x]
            idx[y * W + x] = 1 + RESERVED + (table[c] if table is not None
                                             else qd[k])
            k += 1
    # the quantiser's run is in the same order as the scan, so k only
    # advances on baked pixels - which is what the branch above does

def rgb555(c):
    return 0x8000 | (c[0] >> 3) | ((c[1] >> 3) << 5) | ((c[2] >> 3) << 10)

out = ["/* Generated by tools/make_cassette.py - do not edit by hand.",
       " *",
       " * One index a pixel into the palette below. Index 0 is nothing:",
       " * the ground outside the rounded corners, and the two portholes",
       " * the spools turn behind. Both are drawn live.",
       " *",
       " * Indices 1..%d are not colours yet. They are steps along three" % RESERVED,
       " * ramps - shell to label, label to paper, label to window - and",
       " * the label end of all three is the theme's accent, filled in at",
       " * run time by Skin_TapePalette(). The entries below are zero.",
       " */",
       "#define CASSETTE_W %d" % W,
       "#define CASSETTE_H %d" % H,
       "#define CASSETTE_CY %d" % CY,
       "#define CASSETTE_SPX_L %d" % SPX[0],
       "#define CASSETTE_SPX_R %d" % SPX[1],
       "#define CASSETTE_WINR %d" % WINR,
       "#define CASSETTE_STRIP_X %d" % STRIP[0],
       "#define CASSETTE_STRIP_Y %d" % STRIP[1],
       "#define CASSETTE_STRIP_X1 %d" % STRIP[2],
       "#define CASSETTE_STRIP_Y1 %d" % STRIP[3],
       "#define CASSETTE_TITLE_X %d" % (BADGE[2] + 9),
       "#define CASSETTE_BRAND_X %d" % 22,
       "#define CASSETTE_BRAND_Y %d" % 23,
       "#define CASSETTE_TYPE_X %d" % 196,
       "#define CASSETTE_TYPE_Y %d" % 95,
       "#define CASSETTE_TYPE_W %d" % 18,
       "#define CASSETTE_PANEL_X1 %d" % PANEL[2],
       "#define CASSETTE_PANEL_Y %d" % PANEL[1],
       "",
       "/* The ramps: first index, count, and the colour at the far end. */",
       "#define TAPE_RAMP_N     %d" % RESERVED,
       "#define TAPE_RAMP_SHELL %d" % R_SHELL,
       "#define TAPE_N_SHELL    %d" % N_SHELL,
       "#define TAPE_RAMP_PAPER %d" % R_PAPER,
       "#define TAPE_N_PAPER    %d" % N_PAPER,
       "#define TAPE_RAMP_BAND  %d" % R_BAND,
       "#define TAPE_RAMP_HDR   %d" % R_HDR,
       "#define TAPE_N_HDR      %d" % N_HDR,
       "#define TAPE_N_BAND     %d" % N_BAND,
       "#define TAPE_C_SHELL    0x%04X" % rgb555(SHELL),
       "#define TAPE_C_PAPER    0x%04X" % rgb555(PAPER),
       "#define TAPE_C_BAND     0x%04X" % rgb555(BANDC),
       "#define TAPE_C_INK      0x%04X" % rgb555(INK),
       "",
       "static const unsigned short kCassettePal[%d] = {" % len(newpal)]
for i in range(0, len(newpal), 8):
    out.append("    " + ", ".join("0x%04X" % rgb555(c) for c in newpal[i:i+8]) + ",")
out.append("};")
out.append("static const unsigned char kCassette[CASSETTE_W * CASSETTE_H] = {")
for i in range(0, len(idx), 26):
    out.append("    " + ",".join("%2d" % v for v in idx[i:i+26]) + ",")
out.append("};")

# ---------------------------------------------------------------------
# The hub, at twelve rotations.
#
# White, because it sits on dark tape in a grey window and it is the one
# part of the picture that has to be legible when the spool is empty.
HR, HS = 14, 8
HD = HR * 2 + 1
TEETH, STEP = 8, 3.75            # a tooth every 45 degrees, twelve frames
                                 # of 3.75 carries it exactly one tooth on

HUB_FACE  = (250, 250, 252)
HUB_WELL  = (20, 20, 24)

# The rim needs alpha, not a colour key.
#
# A sprite with one transparent index has to be drawn against something,
# and this one is drawn against two things: the dark of an empty window
# and the pale of a full spool. Antialiased against the dark, its rim
# left a dotted ring of dark pixels every time it sat on tape - the edge
# pixels were blends with a background that was not there any more.
#
# So the rim carries coverage instead. Indices 1..HUB_EDGE mean "white,
# this much of it", and the player blends them with whatever the spool
# put underneath.
HUB_EDGE = 4

frames, edges = [], []
for f in range(12):
    h = Image.new("RGB", (HD * HS, HD * HS), (0, 0, 0))
    hd = ImageDraw.Draw(h)
    c = HR * HS + HS // 2
    ang = f * STEP

    cover = Image.new("L", (HD * HS, HD * HS), 0)
    ImageDraw.Draw(cover).ellipse(
        [c - HR*HS, c - HR*HS, c + HR*HS, c + HR*HS], fill=255)
    edges.append(cover.resize((HD, HD), Image.BOX))

    # a white ring, a dark well inside it, and teeth reaching in across
    # the well to a dark centre - which is the shape a cassette hub is
    # drawn as, rather than the spoked wheel it photographs as
    hd.ellipse([c - 15.0*HS, c - 15.0*HS, c + 15.0*HS, c + 15.0*HS], fill=HUB_FACE)
    hd.ellipse([c - 11.0*HS, c - 11.0*HS, c + 11.0*HS, c + 11.0*HS], fill=HUB_WELL)

    for t in range(TEETH):
        a = math.radians(ang + t * (360.0 / TEETH))
        for rr in range(46, 114):
            r = rr / 10.0
            x = c + math.cos(a) * r * HS
            y = c + math.sin(a) * r * HS
            w = 2.5 * HS
            hd.ellipse([x - w/2, y - w/2, x + w/2, y + w/2], fill=HUB_FACE)

    hd.ellipse([c - 4.8*HS, c - 4.8*HS, c + 4.8*HS, c + 4.8*HS], fill=HUB_WELL)

    frames.append(h.resize((HD, HD), Image.BOX))

sheet = Image.new("RGB", (HD * 12, HD))
for i, h in enumerate(frames):
    sheet.paste(h, (i * HD, 0))

# the interior, quantised; the rim, kept as coverage
NHUB = 31 - HUB_EDGE
hq = sheet.convert("P", palette=Image.ADAPTIVE, colors=NHUB)
hpal = hq.getpalette()[: NHUB * 3]
hraw = list(hq.getdata())
hnew = [(0, 0, 0)] + [HUB_FACE] * HUB_EDGE + \
       [tuple(hpal[i*3:i*3+3]) for i in range(NHUB)]

hidx = [0] * (HD * 12 * HD)
for f in range(12):
    ep = edges[f].load()
    for y in range(HD):
        for x in range(HD):
            a = ep[x, y]
            o = y * (HD * 12) + f * HD + x
            if a == 0:
                hidx[o] = 0
            elif a >= 250:
                hidx[o] = 1 + HUB_EDGE + hraw[o]
            else:
                hidx[o] = max(1, min(HUB_EDGE, (a * HUB_EDGE + 127) // 255))

out.append("")
out.append("/* The hub, twelve rotations of three and three quarter degrees -")
out.append(" * eight teeth repeat every forty-five, so twelve frames carries it")
out.append(" * exactly one tooth on and the step is invisible at the wrap.")
out.append(" * Index 0 is transparent. */")
out.append("#define HUB_D      %d" % HD)
out.append("#define HUB_FRAMES 12")
out.append("#define HUB_EDGE   %d   /* 1..HUB_EDGE are coverage, not colour */" % HUB_EDGE)
out.append("static const unsigned short kHubPal[%d] = {" % len(hnew))
for i in range(0, len(hnew), 8):
    out.append("    " + ", ".join("0x%04X" % rgb555(c) for c in hnew[i:i+8]) + ",")
out.append("};")
out.append("static const unsigned char kHub[HUB_FRAMES * HUB_D * HUB_D] = {")
for f in range(12):
    for y in range(HD):
        row = [hidx[y * (HD * 12) + f * HD + x] for x in range(HD)]
        out.append("    " + ",".join("%2d" % v for v in row) + ",")
out.append("};")

open(sys.argv[1] if len(sys.argv) > 1 else "source/cassette.h", "w").write("\n".join(out) + "\n")
print("wrote %dx%d, %d reserved + %d baked colours"
      % (W, H, RESERVED, len(spare)))
