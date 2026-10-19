#!/usr/bin/env python3
"""Renders the deck for the gramophone card and bakes it.

Two pictures. The first is everything that does not turn: the platter
with its strobe dots, the record sitting on it, the grooves, the bands
between tracks, and the sheen. A record spins, but its grooves are
concentric and the light in the room does not go round with it, so this
is not a shortcut - it is what the thing looks like.

The second is the label, at twelve rotations, because that is the only
part of a record you can watch turning.

Everything is drawn at eight times the size and box-filtered down, so
every curve carries true coverage at its edge rather than a staircase.
Nothing here is allowed a hard edge: the platter's rim, the label's rim
and the label's printing all keep their coverage into the baked picture,
and the player blends them with whatever is underneath.

The label takes the theme's colour, so its pixels are not colours at
all: coverage of the accent at the rim, the accent itself inside, and
four steps from the accent to an ink chosen at run time from how bright
that accent turned out to be.
"""
from PIL import Image, ImageDraw, ImageFilter
import math, sys

S = 8                           # supersample
PR   = 78                       # the platter
RR   = 71                       # the record on it
LABR = 24                       # the label, a hole in this picture
D    = PR * 2 + 1

# ---- what it is all made of -----------------------------------------
VINYL   = (13, 13, 15)
GROOVE  = (34, 34, 38)          # what light catches on the groove walls
BAND    = (54, 54, 60)          # the gap between two tracks
LEADIN  = (22, 22, 26)
EDGELIT = (68, 68, 76)          # the record's rim, catching the light
MAT     = (38, 38, 43)          # the platter's felt
PLATTER = (62, 63, 70)          # and its machined rim
PLATLIT = (92, 94, 103)
STROBE  = (124, 126, 136)
SHADOW  = (8, 8, 10)

# ---- the label's reserved indices -----------------------------------
# Eight steps of coverage, not four.
#
# Four is enough to stop an edge being a staircase and not enough to
# stop it being a stair rail: on a circle this big the four levels sit
# in visible bands around the rim. Eight costs four more palette
# entries and is the difference between an edge you can see the
# construction of and one you cannot.
EDGE   = 8                      # 1..8   coverage of the accent, at the rim
I_FACE = 9                      # the accent itself
I_INK  = 10                     # 10..13 four steps from accent to ink
INK_N  = 4
I_PIN  = 14                     # 14..16 the spindle standing in the middle
PIN_N  = 3
LAB_N  = I_PIN + PIN_N          # 0..16

def rgb555(c):
    return 0x8000 | (c[0] >> 3) | ((c[1] >> 3) << 5) | ((c[2] >> 3) << 10)

# ===================================================================
# the deck
# ===================================================================
im = Image.new("RGB", (D * S, D * S), (0, 0, 0))
dd = ImageDraw.Draw(im)
c = PR * S + S // 2

def disc(d, r, fill, cx=None, cy=None):
    cx = c if cx is None else cx
    cy = c if cy is None else cy
    d.ellipse([cx - r * S, cy - r * S, cx + r * S, cy + r * S], fill=fill)

# --- the platter: a machined rim, a lit chamfer, then the felt mat
disc(dd, PR, PLATTER)
disc(dd, PR - 1.0, PLATLIT)
disc(dd, PR - 2.4, PLATTER)
disc(dd, PR - 6, MAT)

# Two rows of strobe dots, the outer one twice as fine as the inner -
# which is what a deck carries so that one row stands still at 33 and the
# other at 45. It is the detail that says turntable rather than plate.
for (n, rad, w) in ((60, PR - 2.4, 1.1), (30, PR - 4.8, 1.6)):
    for i in range(n):
        a = math.radians(i * 360.0 / n)
        x = c + math.cos(a) * rad * S
        y = c + math.sin(a) * rad * S
        dd.ellipse([x - w * S / 2, y - w * S / 2,
                    x + w * S / 2, y + w * S / 2], fill=STROBE)

# The felt's own weave: fine rings, much fainter than the record's
# grooves, so the mat reads as cloth rather than as more plastic.
for r in range(RR + 3, PR - 6):
    if (r % 2) == 0:
        disc(dd, r, (MAT[0] + 5, MAT[1] + 5, MAT[2] + 6))
        disc(dd, r - 0.9, MAT)

# --- the record's shadow on the mat, offset the way the light falls
disc(dd, RR + 1.6, SHADOW, cx=c + 1.4 * S, cy=c + 1.8 * S)

# --- the record
disc(dd, RR, EDGELIT)
disc(dd, RR - 1.1, LEADIN)
disc(dd, RR - 4, VINYL)

# The grooves, as a texture rather than as lines you could count. A
# three pixel period is the finest that survives the box filter: at two
# the light and dark rings average each other away and leave a flat grey
# disc, which is what the first attempt at this looked like.
r = LABR + 2
while r < RR - 4:
    if (r % 3) == 0:
        disc(dd, r, GROOVE)
        disc(dd, r - 1.2, VINYL)
    r += 1

# the bands between tracks, unevenly spaced because tracks are not the
# same length, and the lead-out just outside the label
for r in (RR - 7, RR - 16, RR - 26, RR - 35, RR - 43, LABR + 4):
    disc(dd, r, BAND)
    disc(dd, r - 1.3, VINYL)

# The run-out: the smooth band between the last groove and the label,
# where a record has no music on it at all. Slightly darker than the
# grooved part, because there is nothing there to catch the light.
disc(dd, LABR + 3, (VINYL[0] + 3, VINYL[1] + 3, VINYL[2] + 4))
disc(dd, LABR + 0.8, (9, 9, 11))

# --- the sheen: two narrow streaks where the light catches the groove
# walls, opposite each other the way they are under one lamp. Narrow and
# bright rather than broad and dim - broad and dim is fog.
sheen = Image.new("L", (D * S, D * S), 0)
sd = ImageDraw.Draw(sheen)
for (y0, y1, a) in ((34, 48, 50), (116, 128, 34)):
    sd.polygon([(-10 * S, y0 * S), ((D + 10) * S, (y0 - 44) * S),
                ((D + 10) * S, (y1 - 44) * S), (-10 * S, y1 * S)], fill=a)
sheen = sheen.filter(ImageFilter.GaussianBlur(radius=S * 1.4))
m = Image.new("L", (D * S, D * S), 0)
disc(ImageDraw.Draw(m), RR - 1, 255)
sheen = Image.composite(sheen, Image.new("L", (D * S, D * S), 0), m)
im.paste(Image.new("RGB", (D * S, D * S), (255, 255, 255)), (0, 0), sheen)

# --- coverage: the platter's outer edge only.
#
# The label's hole is NOT cut here. Cut with coverage, its edge took the
# same indices as the platter's rim - which blend toward the platter's
# grey - so the label wore a grey halo on one side; and where the hole's
# soft edge and the label's own soft edge both fell short, the card's
# ground showed through as a dark crescent on the other. Two antialiased
# edges cannot be laid against each other and asked to add up.
#
# So the vinyl is drawn solid right up to under the label, the hole is
# cut hard a couple of pixels inside it, and the label's rim does all the
# blending - against vinyl, which is what is actually behind it.
cover = Image.new("L", (D * S, D * S), 0)
cd = ImageDraw.Draw(cover)
disc(cd, PR, 255)

im = im.resize((D, D), Image.BOX)
cover = cover.resize((D, D), Image.BOX)

# ---- bake -----------------------------------------------------------
ip, cp = im.load(), cover.load()
DISC_EDGE = 8
flat, idx = [], [0] * (D * D)
HOLE2 = (LABR - 2) * (LABR - 2)
for y in range(D):
    for x in range(D):
        hx, hy = x - PR, y - PR
        if hx * hx + hy * hy < HOLE2:
            continue                      # the label's hole, cut hard
        a = cp[x, y]
        if a == 0:
            continue
        if a < 250:
            idx[y * D + x] = max(1, min(DISC_EDGE, (a * DISC_EDGE + 127) // 255))
        else:
            idx[y * D + x] = -1 - len(flat)
            flat.append(ip[x, y])

SPARE = 63 - DISC_EDGE
tmp = Image.new("RGB", (len(flat), 1)); tmp.putdata(flat)
q = tmp.convert("P", palette=Image.ADAPTIVE, colors=SPARE,
                dither=Image.Dither.NONE)
qp = q.getpalette()[: SPARE * 3]
qd = list(q.getdata())
pal = [(0, 0, 0)] + [PLATTER] * DISC_EDGE + \
      [tuple(qp[i * 3:i * 3 + 3]) for i in range(SPARE)]
for i, v in enumerate(idx):
    if v < 0:
        idx[i] = 1 + DISC_EDGE + qd[-1 - v]

out = ["/* Generated by tools/make_gramophone.py - do not edit by hand.",
       " *",
       " * The deck: platter, strobe dots, and the record on it, one",
       " * index a pixel. Index 0 is nothing - outside the platter, and",
       " * the hole the label turns in. Indices 1..%d are coverage of" % DISC_EDGE,
       " * the platter's rim, which the player blends with whatever",
       " * ground the theme put behind it.",
       " */",
       "#define RECORD_D    %d" % D,
       "#define RECORD_R    %d" % RR,
       "#define RECORD_PR   %d" % PR,
       "#define RECORD_LABR %d" % LABR,
       "#define RECORD_EDGE %d" % DISC_EDGE,
       "#define RECORD_C_RIM 0x%04X" % rgb555(PLATTER),
       "static const unsigned short kRecordPal[%d] = {" % len(pal)]
for i in range(0, len(pal), 8):
    out.append("    " + ", ".join("0x%04X" % rgb555(v) for v in pal[i:i + 8]) + ",")
out.append("};")
out.append("static const unsigned char kRecord[RECORD_D * RECORD_D] = {")
for i in range(0, len(idx), 26):
    out.append("    " + ",".join("%2d" % v for v in idx[i:i + 26]) + ",")
out.append("};")

# ===================================================================
# the label
# ===================================================================
LD = LABR * 2 + 1
LS = 8
frames = []

for f in range(12):
    ang = f * 30.0
    lc = LABR * LS + LS // 2

    ink = Image.new("L", (LD * LS, LD * LS), 0)
    kd = ImageDraw.Draw(ink)

    def dot(x, y, w):
        kd.ellipse([x - w * LS / 2, y - w * LS / 2,
                    x + w * LS / 2, y + w * LS / 2], fill=255)

    def arc(a0, rad, span, w):
        """A mark lying along the arc at `rad`, `span` degrees of it."""
        steps = max(3, int(span * 3))
        for i in range(steps + 1):
            a = math.radians(ang + a0 + span * i / steps)
            dot(lc + math.cos(a) * rad * LS, lc + math.sin(a) * rad * LS, w)

    # the printed ring
    kd.ellipse([lc - (LABR - 3) * LS, lc - (LABR - 3) * LS,
                lc + (LABR - 3) * LS, lc + (LABR - 3) * LS],
               outline=255, width=max(1, int(0.7 * LS)))

    # Two arcs of lettering set round it - the shape a label's type takes.
    # Not a ring of even ticks: that came out as a clock face, hand and all.
    at = -152
    for span in (16, 10, 20, 12, 8):
        arc(at, LABR - 7, span, 1.7)
        at += span + 8
    ab = 40
    for span in (12, 18, 10, 14):
        arc(ab, LABR - 7, span, 1.5)
        ab += span + 9

    # two rules, one under each arc, and the solid mark that makes the
    # turning obvious at a glance
    arc(-150, LABR - 11, 96, 0.9)
    arc(38, LABR - 11, 62, 0.8)

    # the small print a label carries round the bottom of its ring
    at2 = 150
    for span in (7, 5, 9):
        arc(at2, LABR - 17, span, 1.2)
        at2 += span + 7
    for rr in range(int((LABR - 15) * 10), int((LABR - 6) * 10)):
        rad = rr / 10.0
        a = math.radians(ang)
        dot(lc + math.cos(a) * rad * LS, lc + math.sin(a) * rad * LS, 2.6)

    # The face is solid to the middle: the spindle hole is only kept
    # clear of the printing. Punched through the face as well, it let the
    # card's ground through in a ring around the spindle pin - the hole
    # in a record shows the deck, and the deck is not what is behind this
    # picture.
    face = Image.new("L", (LD * LS, LD * LS), 0)
    fd = ImageDraw.Draw(face)
    fd.ellipse([lc - LABR * LS, lc - LABR * LS,
                lc + LABR * LS, lc + LABR * LS], fill=255)
    kd.ellipse([lc - 4.5 * LS, lc - 4.5 * LS,
                lc + 4.5 * LS, lc + 4.5 * LS], fill=0)

    # The spindle pin, baked into every frame of the label rather than
    # drawn on top of it.
    #
    # It stands still while the label turns, so it is the same three
    # rings in all twelve - but drawn on top it was three circles plotted
    # a pixel at a time on every repaint, which was the last per-pixel
    # work left on this card. Baked, it costs nothing: it arrives with
    # the blit that was happening anyway.
    pin = Image.new("L", (LD * LS, LD * LS), 0)
    pd2 = ImageDraw.Draw(pin)
    for (rr, v) in ((3.6, I_PIN), (2.3, I_PIN + 1), (1.0, I_PIN + 2)):
        pd2.ellipse([lc - rr * LS, lc - rr * LS,
                     lc + rr * LS, lc + rr * LS], fill=v)

    frames.append((face.resize((LD, LD), Image.BOX),
                   ink.resize((LD, LD), Image.BOX),
                   pin.resize((LD, LD), Image.NEAREST)))

lidx = [0] * (12 * LD * LD)
for f, (face, ink, pin) in enumerate(frames):
    fp, kp, pnp = face.load(), ink.load(), pin.load()
    for y in range(LD):
        for x in range(LD):
            o = f * LD * LD + y * LD + x
            if pnp[x, y]:
                lidx[o] = pnp[x, y]
                continue
            a = fp[x, y]
            if a == 0:
                lidx[o] = 0
            elif a < 250:
                lidx[o] = max(1, min(EDGE, (a * EDGE + 127) // 255))
            else:
                k = kp[x, y]
                lidx[o] = I_FACE if k == 0 else \
                          I_INK - 1 + max(1, min(INK_N, (k * INK_N + 127) // 255))

out.append("")
out.append("/* The label, twelve rotations of thirty degrees.")
out.append(" *")
out.append(" * Not colours. 1..%d are coverage of the accent at the rim," % EDGE)
out.append(" * %d is the accent itself, and %d..%d are four steps from the"
           % (I_FACE, I_INK, I_INK + INK_N - 1))
out.append(" * accent to the ink - so the printing has a soft edge rather")
out.append(" * than a staircase, which at this size is the difference")
out.append(" * between type and dirt. Index 0 is transparent: outside the")
out.append(" * label, and the spindle hole where the disc shows through. */")
out.append("#define LABEL_D      %d" % LD)
out.append("#define LABEL_FRAMES 12")
out.append("#define LABEL_EDGE   %d" % EDGE)
out.append("#define LABEL_FACE   %d" % I_FACE)
out.append("#define LABEL_INK    %d" % I_INK)
out.append("#define LABEL_INK_N  %d" % INK_N)
out.append("#define LABEL_PIN    %d" % I_PIN)
out.append("#define LABEL_PIN_N  %d" % PIN_N)
out.append("#define LABEL_PAL_N  %d" % LAB_N)
out.append("static const unsigned char kLabel[LABEL_FRAMES * LABEL_D * LABEL_D] = {")
for f in range(12):
    for y in range(LD):
        row = lidx[f * LD * LD + y * LD: f * LD * LD + (y + 1) * LD]
        out.append("    " + ",".join("%2d" % v for v in row) + ",")
out.append("};")

# ===================================================================
# the plinth the deck stands in
# ===================================================================
#
# A wooden box with a well turned in it for the platter, which is what a
# deck of this shape actually is. Baked whole, at the size of the screen,
# for two reasons: grain is noise and noise cannot be drawn cheaply at
# run time; and everything moulded into it - the well, the control
# recesses, the hinge blocks - then comes back for free whenever a piece
# of the card is put back, instead of having to be painted over the top
# and dodged by every restore box.
#
# Fixed colours, not the theme's: wood is wood. The theme still owns the
# label, the lamp and the name across the top.
BW, BH = 256, 192
BCX, BCY = 128, 100             # where the deck is centred on it
BS = 4                          # supersample, for the moulded shapes only

WOOD      = (78, 52, 33)
WOOD_DARK = (56, 37, 23)
WOOD_LIT  = (100, 70, 46)
WELL      = (26, 18, 12)
WELL_LIT  = (96, 68, 44)
RECESS    = (36, 25, 16)
METAL     = (58, 58, 64)
METAL_LIT = (104, 106, 114)

def lerp3(a, b, t):
    t = 0.0 if t < 0 else (1.0 if t > 1 else t)
    return tuple(int(a[i] + (b[i] - a[i]) * t + 0.5) for i in range(3))

# --- the grain, at final size: it is texture, and supersampling texture
# only averages it away
bg = Image.new("RGB", (BW, BH))
bp = bg.load()
for y in range(BH):
    for x in range(BW):
        # A wavy coordinate, so the grain runs in lines that are not
        # straight - straight lines read as corduroy, not as timber -
        # and a coordinate whose pitch varies down the board, because
        # evenly spaced lines read as corduroy too. The first attempt at
        # this had both faults and came out as corrugated card.
        t = (y * 0.62
             + 7.0 * math.sin(x * 0.011)
             + 2.5 * math.sin(x * 0.043 + 1.7)
             + 1.2 * math.sin(x * 0.091 + 0.4)
             + 3.4 * math.sin(y * 0.031 + 2.2)
             + 1.1 * math.sin(y * 0.13 + 0.8))
        g = (math.sin(t) * 0.5 + 0.5) ** 2.4
        # fibre: noise stretched along the grain, not round dots
        fib = ((((x * 2654435761) >> 7) ^ (y * 40503)) % 211) / 211.0 - 0.5
        fib += (((x * 7919 + (y // 3) * 104729) % 101) / 101.0 - 0.5)
        n = fib * 0.5
        c = lerp3(WOOD, WOOD_DARK, g * 0.46)
        c = lerp3(c, WOOD_LIT, max(0.0, 0.16 - g) * 0.9)
        # a little light from the top left, and the corners falling away
        dx, dy = (x - 90) / 200.0, (y - 60) / 160.0
        v = 1.0 - min(1.0, (dx * dx + dy * dy)) * 0.42

        # The seam where two leaves of veneer meet. A hairline, not a
        # gap: at full strength it read as two planks with daylight
        # between them rather than as one veneered box.
        if abs(x - 71) < 1:
            v *= 0.93

        # the lit top and left edges of the box, and the dark under them
        if y < 3 or x < 3:          v *= 1.28
        elif y < 5 or x < 5:        v *= 1.10
        if y > BH - 4 or x > BW - 4: v *= 0.72
        elif y > BH - 6 or x > BW - 6: v *= 0.86

        c = tuple(max(0, min(255, int(ch * v + n * 7))) for ch in c)
        bp[x, y] = c

# --- the moulded shapes, supersampled so their edges are curves
sh = Image.new("RGB", (BW * BS, BH * BS), (0, 0, 0))
ma = Image.new("L", (BW * BS, BH * BS), 0)
shd, mad = ImageDraw.Draw(sh), ImageDraw.Draw(ma)

def well(r, fill):
    shd.ellipse([(BCX - r) * BS, (BCY - r) * BS,
                 (BCX + r) * BS, (BCY + r) * BS], fill=fill)
    mad.ellipse([(BCX - r) * BS, (BCY - r) * BS,
                 (BCX + r) * BS, (BCY + r) * BS], fill=255)

def pad(x0, y0, x1, y1, rad, fill):
    shd.rounded_rectangle([x0 * BS, y0 * BS, (x1 + 1) * BS - 1, (y1 + 1) * BS - 1],
                          radius=rad * BS, fill=fill)
    mad.rounded_rectangle([x0 * BS, y0 * BS, (x1 + 1) * BS - 1, (y1 + 1) * BS - 1],
                          radius=rad * BS, fill=255)

# the well the platter turns in: a thin lit chamfer where the light
# catches the cut edge, then the dark of the box
well(PR + 4.2, WELL_LIT)
well(PR + 3.2, WELL)

# the recesses the controls sit in, and the blocks the dust cover hinges on
for (yy) in (52, 76):
    pad(8, yy, 42, yy + 20, 2, RECESS)
    pad(9, yy + 1, 41, yy + 19, 2, METAL)
pad(8, 106, 42, 128, 2, RECESS)
pad(9, 107, 41, 127, 2, METAL)
pad(14, 112, 36, 122, 1, (20, 20, 24))
pad(20, 138, 30, 172, 2, RECESS)
pad(24, 142, 26, 168, 1, (18, 18, 21))
# and no knob on it: the player draws that, where the volume is
for hx in (18, BW - 46):
    pad(hx, 0, hx + 27, 6, 1, (26, 26, 30))
    pad(hx + 1, 0, hx + 26, 5, 1, METAL)
    pad(hx + 1, 0, hx + 26, 1, 0, METAL_LIT)
    pad(hx + 5, 2, hx + 22, 4, 1, (20, 20, 24))
    pad(hx + 7, 3, hx + 20, 3, 0, (64, 64, 70))

# The screws that hold the plinth together, sunk in their own dimples.
# Four of them, at the corners, where a deck's actually are.
for (sx2, sy2) in ((11, 11), (BW - 12, 11), (11, BH - 12), (BW - 12, BH - 12)):
    shd.ellipse([(sx2 - 4) * BS, (sy2 - 4) * BS,
                 (sx2 + 4) * BS, (sy2 + 4) * BS], fill=(44, 30, 19))
    mad.ellipse([(sx2 - 4) * BS, (sy2 - 4) * BS,
                 (sx2 + 4) * BS, (sy2 + 4) * BS], fill=190)
    shd.ellipse([(sx2 - 3) * BS, (sy2 - 3) * BS,
                 (sx2 + 3) * BS, (sy2 + 3) * BS], fill=(96, 98, 106))
    mad.ellipse([(sx2 - 3) * BS, (sy2 - 3) * BS,
                 (sx2 + 3) * BS, (sy2 + 3) * BS], fill=255)
    shd.ellipse([(sx2 - 2.4) * BS, (sy2 - 2.4) * BS,
                 (sx2 + 2.4) * BS, (sy2 + 2.4) * BS], fill=(62, 63, 70))
    # the cross head, which is what makes a screw read as a screw
    shd.rectangle([(sx2 - 2.2) * BS, (sy2 - 0.4) * BS,
                   (sx2 + 2.2) * BS, (sy2 + 0.6) * BS], fill=(30, 30, 34))
    shd.rectangle([(sx2 - 0.4) * BS, (sy2 - 2.2) * BS,
                   (sx2 + 0.6) * BS, (sy2 + 2.2) * BS], fill=(30, 30, 34))

sh = sh.resize((BW, BH), Image.BOX)
ma = ma.resize((BW, BH), Image.BOX)
shp, map_ = sh.load(), ma.load()

wood = bg.copy()                # the grain on its own, before the metal
for y in range(BH):
    for x in range(BW):
        a = map_[x, y]
        if a:
            bp[x, y] = lerp3(bp[x, y], shp[x, y], a / 255.0)

# ---- bake -----------------------------------------------------------
# A hundred and twenty eight, not sixty four.
#
# The quantiser hands out entries in proportion to area, and this picture
# is six parts wood to one part anything else - so at sixty four the
# controls got nine colours between them and came out as brown mud. The
# extra sixty four entries cost a hundred and twenty eight bytes and are
# nearly all spent on the metal.
#
# And quantised in two passes, wood and metal separately, because one
# pass will not do it at any palette size.
#
# Median cut hands out its boxes by population, not by error. The metal
# on this plinth is about one pixel in forty, so it never earns a box of
# its own: every grey in the controls gets folded into the nearest
# brown. At sixty four colours that was mud; at a hundred and twenty
# eight it was exactly the same mud, which is what said the size of the
# palette was not the problem. The slider's knob came out with a brown
# stripe across it that is in no layer of the drawing.
#
# So the grain is quantised alone, the pixels the metal covers are
# quantised alone, and the two palettes are laid end to end.
#
# No dithering either way: convert() dithers by default, and on flat
# machined surfaces that is not a help but a fault.
NWOOD, NMETAL = 96, 32
NBACK = NWOOD + NMETAL

wq = wood.convert("P", palette=Image.ADAPTIVE, colors=NWOOD,
                  dither=Image.Dither.NONE)
wpal = wq.getpalette()[: NWOOD * 3]
widx = list(wq.getdata())

metal = [bg.getpixel((x, y)) for y in range(BH) for x in range(BW)
         if map_[x, y]]
mt = Image.new("RGB", (len(metal), 1)); mt.putdata(metal)
mq = mt.convert("P", palette=Image.ADAPTIVE, colors=NMETAL,
                dither=Image.Dither.NONE)
mpal = mq.getpalette()[: NMETAL * 3]
midx = list(mq.getdata())

bpal = wpal + mpal
bidx = [0] * (BW * BH)
k = 0
for y in range(BH):
    for x in range(BW):
        o = y * BW + x
        if map_[x, y]:
            bidx[o] = NWOOD + midx[k]
            k += 1
        else:
            bidx[o] = widx[o]

out.append("")
out.append("/* The plinth: the whole screen, baked.")
out.append(" *")
out.append(" * Grain is noise, and noise cannot be drawn cheaply at run")
out.append(" * time. Baking it whole also means everything moulded into")
out.append(" * it - the well the platter turns in, the control recesses,")
out.append(" * the hinge blocks - comes back for free whenever a piece of")
out.append(" * the card is put back, rather than being painted over the")
out.append(" * top once and dodged by every restore box afterwards.")
out.append(" *")
out.append(" * Fixed colours, not the theme's. Wood is wood; the theme")
out.append(" * still owns the label, the lamp and the name across the top. */")
out.append("#define BACK_W %d" % BW)
out.append("#define BACK_H %d" % BH)
out.append("#define BACK_PAL_N %d" % NBACK)
out.append("/* Entries below this are grain and take the theme's")
out.append(" * tint; the rest are metal and do not. */")
out.append("#define BACK_WOOD_N %d" % NWOOD)
out.append("static const unsigned short kBackPal[%d] = {" % NBACK)
for i in range(0, NBACK, 8):
    out.append("    " + ", ".join("0x%04X" % rgb555(tuple(bpal[j*3:j*3+3]))
                                  for j in range(i, i + 8)) + ",")
out.append("};")
out.append("static const unsigned char kBack[BACK_W * BACK_H] = {")
for i in range(0, len(bidx), 26):
    out.append("    " + ",".join("%2d" % v for v in bidx[i:i + 26]) + ",")
out.append("};")

open(sys.argv[1] if len(sys.argv) > 1 else "source/gramophone.h",
     "w").write("\n".join(out) + "\n")
print("deck %dx%d (record r=%d on platter r=%d), %d colours; label %dx%d x12;"
      " plinth %dx%d"
      % (D, D, RR, PR, len(pal), LD, LD, BW, BH))
