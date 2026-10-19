#!/usr/bin/env python3
"""Builds synthetic but structurally valid MPEG-1 Layer III files.

Every frame is a header plus zeroed side info, which is the standard way
to express a silent frame: part2_3_length is 0 for both granules, so no
spectral data is read and the decoder outputs silence. That is enough to
exercise the decoder's framing, EOF, duration and seek logic without
needing an encoder on the machine.
"""
import struct
import sys

SAMPLE_RATE = 44100
SAMPLES_PER_FRAME = 1152
BITRATE = 128000
FRAME_LEN = 144 * BITRATE // SAMPLE_RATE   # 417 bytes for 128k/44.1k

# MPEG1, layer III, no CRC, 128kbps, 44100Hz, stereo
HEADER = bytes([0xFF, 0xFB, 0x90, 0x00])
SIDE_INFO_STEREO = 32


def silent_frame():
    return HEADER + bytes(FRAME_LEN - 4)


def xing_frame(total_frames, total_bytes, with_toc=True):
    """The Xing header lives in the first frame, after the side info."""
    body = bytearray(FRAME_LEN - 4)
    off = SIDE_INFO_STEREO          # relative to just after the 4 byte header

    flags = 0x1 | 0x2 | (0x4 if with_toc else 0)
    blob = b"Xing" + struct.pack(">I", flags)
    blob += struct.pack(">I", total_frames)
    blob += struct.pack(">I", total_bytes)
    if with_toc:
        # linear table of contents: entry i covers i% of the byte range
        blob += bytes(min(255, (i * 256) // 100) for i in range(100))

    body[off:off + len(blob)] = blob
    return HEADER + bytes(body)


def id3v2_frame(fid, text, enc=0):
    """One ID3v2.3/2.4 text frame. enc 0=Latin-1, 1=UTF-16+BOM, 3=UTF-8."""
    if enc == 1:
        body = bytes([1]) + b"\xff\xfe" + text.encode("utf-16-le") + b"\x00\x00"
    elif enc == 3:
        body = bytes([3]) + text.encode("utf-8") + b"\x00"
    else:
        body = bytes([0]) + text.encode("latin-1", "replace") + b"\x00"
    n = len(body)
    # syncsafe size, which is what a 2.4 tag is supposed to carry
    size = bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])
    return fid.encode() + size + b"\x00\x00" + body


def id3v2_frame_raw(fid, body):
    n = len(body)
    size = bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])
    return fid.encode() + size + b"\x00\x00" + body


def id3v1(title, artist):
    t = bytearray(b"TAG" + b"\x00" * 125)
    t[3:3 + len(title)] = title.encode("latin-1", "replace")[:30]
    t[33:33 + len(artist)] = artist.encode("latin-1", "replace")[:30]
    return bytes(t)


def build(path, frames, xing=False, id3=False, tags=None, v1=None, art=0):
    out = bytearray()

    if id3 or tags:
        body = bytearray()
        # Album art first, deliberately: real taggers put APIC before the
        # text frames, and a reader that only looks at the first few
        # kilobytes of a tag would miss the title on exactly those files.
        if art:
            body += b"APIC" + bytes([(art >> 21) & 0x7F, (art >> 14) & 0x7F,
                                     (art >> 7) & 0x7F, art & 0x7F])
            body += b"\x00\x00" + bytes(art)
        for fid, text, enc in (tags or []):
            body += id3v2_frame(fid, text, enc)
        body += bytes(1000 if not tags else 16)      # padding
        size = len(body)
        syncsafe = bytes([(size >> 21) & 0x7F, (size >> 14) & 0x7F,
                          (size >> 7) & 0x7F, size & 0x7F])
        out += b"ID3" + bytes([4, 0, 0]) + syncsafe + bytes(body)

    audio_frames = frames
    total_bytes = FRAME_LEN * (frames + (1 if xing else 0))

    if xing:
        out += xing_frame(audio_frames, total_bytes)

    out += silent_frame() * audio_frames

    if v1:
        out += id3v1(*v1)

    with open(path, "wb") as f:
        f.write(out)

    duration_ms = audio_frames * SAMPLES_PER_FRAME * 1000 // SAMPLE_RATE
    print(f"{path}: {frames} frames, {len(out)} bytes, "
          f"{duration_ms}ms audio, xing={xing} id3={id3}")
    return duration_ms


if __name__ == "__main__":
    d = sys.argv[1] if len(sys.argv) > 1 else "."
    build(f"{d}/plain.mp3", 200)
    build(f"{d}/xing.mp3", 200, xing=True)
    build(f"{d}/id3.mp3", 200, id3=True)

    # Tag-reading fixtures.
    # Latin-1 can only carry Latin-1: an en dash here would be mangled by
    # the tagger, not by us, so the accent folding is what this tests.
    build(f"{d}/tag_latin1.mp3", 40,
          tags=[("TIT2", "Bj\u00f6rk \u00b7 Hyperballad", 0),
                ("TPE1", "Bj\u00f6rk", 0)])
    # UTF-16 can, so the typographic punctuation is tested here.
    build(f"{d}/tag_utf16.mp3", 40,
          tags=[("TIT2", "Caf\u00e9 \u2013 \u201cBleu\u201d\u2026", 1),
                ("TPE1", "The Style Council", 1)])
    build(f"{d}/tag_utf8.mp3", 40,
          tags=[("TIT2", "Se\u00f1or", 3), ("TPE1", "\u4e2d\u6587", 3)])
    # Persian, both UTF-8 and UTF-16, because taggers use both. The title
    # is "Nemidanam" and the artist "Dariush" - two names that used to
    # arrive as a row of question marks. The title carries a harakat and
    # a zero-width non-joiner, which have no letter and must not become
    # characters of their own.
    build(f"{d}/tag_fa.mp3", 40,
          tags=[("TIT2", "\u0646\u0645\u06cc\u200c\u062f\u0627\u0646\u0645", 3),
                ("TPE1", "\u062f\u0627\u0631\u06cc\u0648\u0634", 3)])
    build(f"{d}/tag_fa16.mp3", 40,
          tags=[("TIT2", "\u0686\u0647\u0627\u0631\u0634\u0646\u0628\u0647 \u06f1\u06f4", 1),
                ("TPE1", "\u0632\u0646\u062f\u06af\u06cc", 1)])
    # Art before the text frames, so seeking past big frames is exercised.
    build(f"{d}/tag_art.mp3", 40, art=60000,
          tags=[("TIT2", "Behind The Art", 0), ("TPE1", "Buried", 0)])
    # v1 only, and v1 alongside v2 (v2 must win).
    build(f"{d}/tag_v1.mp3", 40, v1=("Old Tag Title", "Old Tag Artist"))
    build(f"{d}/tag_both.mp3", 40,
          tags=[("TIT2", "From v2", 0)], v1=("From v1", "V1 Artist"))
    build(f"{d}/tag_none.mp3", 40)

    # Real JPEGs in APIC frames.
    #
    # Two of them. The 300 pixel gradient tells a decoder that works from
    # one that produces a plausible but wrong image. The 96 pixel one
    # exists because the thumbnail is capped at 64: at 300 pixels every
    # detail level clamps to the cap and the levels cannot be compared
    # against each other at all. 96 gives 12, 24 and 48 - all under the
    # cap - so the relationship between them is testable.
    #
    # It carries diagonal stripes as well as a gradient, because a smooth
    # ramp puts almost nothing in the higher coefficients and those are
    # exactly the ones the 4x4 path adds.
    import subprocess, os

    def make_jpeg(name, w, h, pixel):
        ppm = f"{d}/_{name}.ppm"
        with open(ppm, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (w, h))
            f.write(bytes(v for y in range(h) for x in range(w)
                          for v in pixel(x, y)))
        jpg = f"{d}/_{name}.jpg"
        subprocess.run(["sips", "-s", "format", "jpeg", ppm, "--out", jpg],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return open(jpg, "rb").read() if os.path.exists(jpg) else None

    def write_art(name, art, title, artist):
        apic = b"\x00" + b"image/jpeg\x00" + b"\x03" + b"Cover\x00" + art
        body = id3v2_frame_raw("APIC", apic)
        body += id3v2_frame("TIT2", title, 0)
        body += id3v2_frame("TPE1", artist, 0)
        n = len(body)
        size = bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F,
                      (n >> 7) & 0x7F, n & 0x7F])
        tag = b"ID3" + bytes([4, 0, 0]) + size + body
        with open(f"{d}/{name}", "wb") as f:
            f.write(tag + silent_frame() * 40)
        print(f"{d}/{name}: cover {len(art)} bytes")

    art = make_jpeg("art", 300, 300,
                    lambda x, y: (x * 255 // 300, y * 255 // 300, 128))
    if art:
        write_art("art.mp3", art, "Art Test", "The Artist")

        # The same picture behind a UTF-16 description.
        #
        # This is the part of an APIC frame that is easiest to walk
        # wrongly: the description ends in two zero bytes rather than
        # one, and a parser that stops at the first leaves the decoder
        # pointing a byte into the JPEG's start marker. A tagger that
        # writes UTF-16 text writes a UTF-16 description with it, so this
        # is not an exotic file - it is what half of them look like.
        def write_art_utf16(name, art, title, artist):
            desc = "Cover".encode("utf-16-le")
            apic = (b"\x01" + b"image/jpeg\x00" + b"\x03" +
                    b"\xff\xfe" + desc + b"\x00\x00" + art)
            body = id3v2_frame_raw("APIC", apic)
            body += id3v2_frame("TIT2", title, 0)
            body += id3v2_frame("TPE1", artist, 0)
            n = len(body)
            size = bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F,
                          (n >> 7) & 0x7F, n & 0x7F])
            tag = b"ID3" + bytes([4, 0, 0]) + size + body
            with open(f"{d}/{name}", "wb") as f:
                f.write(tag + silent_frame() * 40)
            print(f"{d}/{name}: cover {len(art)} bytes, UTF-16 description")

        write_art_utf16("art_utf16.mp3", art, "Wide Desc", "The Artist")

        # And one with a description too long to scan past.
        #
        # The decoder has two ways to find the picture: parse the frame
        # header, or scan the first few hundred bytes for a JPEG start
        # marker. The scan rescues a parse that went wrong, which is why
        # it is there - and it also means a test with a short description
        # passes whether the parse works or not. A description longer
        # than the scan window can only be got past by parsing it.
        def write_art_longdesc(name, art, title):
            desc = ("A very long description " * 40).encode("utf-16-le")
            apic = (b"\x01" + b"image/jpeg\x00" + b"\x03" +
                    b"\xff\xfe" + desc + b"\x00\x00" + art)
            body = id3v2_frame_raw("APIC", apic)
            body += id3v2_frame("TIT2", title, 0)
            n = len(body)
            size = bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F,
                          (n >> 7) & 0x7F, n & 0x7F])
            with open(f"{d}/{name}", "wb") as f:
                f.write(b"ID3" + bytes([4, 0, 0]) + size + body +
                        silent_frame() * 40)
            print(f"{d}/{name}: cover behind a {len(desc)}-byte description")

        write_art_longdesc("art_longdesc.mp3", art, "Long Desc")

    build(f"{d}/tiny.mp3", 3)
