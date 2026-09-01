#!/usr/bin/env python3
"""
Turn an image into a C header for QrStyle_Logo: RGB565 pixels and a 1bpp alpha
mask, both const so they land in flash.

PIXEL ART IS RESAMPLED, NOT SCALED. An avatar exported at 766x1024 is a 33x45
sprite that somebody blew up by 19.24, and carrying the blown-up version into
the firmware would cost 60x the flash for none of the detail -- worse, a JPEG
of it carries ringing around every hard edge, which is exactly the artefact a
QR code's logo cut-out will show off. So the grid period is measured from the
image's own edge energy, one sample is taken per art pixel from the middle of
its cell, and what comes out is the sprite the artist drew.

  --grid auto   measure the period (default), or give it: --grid 19.24. Auto
                maximises edge energy landing on grid lines and takes the
                smallest period that scores near the best, since every multiple
                of the true period scores as well on half the samples. It is a
                heuristic and it can still be fooled -- CHECK THE SPRITE SIZE it
                prints against the artwork before committing the header.
  --phase X,Y   the grid's offset, when you are giving --grid yourself
  --key-checker treat the light/lighter chequerboard a flattened PNG leaves
                behind as transparent, flood-filled from the border so a
                genuinely pale pixel inside the artwork survives

Needs Pillow, and is NOT part of the build: the header it writes is committed.
Run it again when the artwork changes.

  python tools/logo-to-c.py in.png examples/logo_tori.h --name tori
"""

import argparse
import sys
from collections import deque

try:
    from PIL import Image
except ImportError:
    sys.exit("this needs Pillow:  python -m pip install pillow")


def detect_period(px, w, h, lo, hi):
    """The pixel-art grid period, from where the horizontal edge energy piles up."""
    energy = [0] * w
    for x in range(1, w):
        s = 0
        for y in range(0, h, 3):
            a, b = px[x - 1, y], px[x, y]
            s += abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])
        energy[x] = s

    scored = []
    for p100 in range(lo * 100, hi * 100):
        p = p100 / 100.0
        best_ph = None
        for ph10 in range(0, int(p * 10)):
            phase = ph10 / 10.0
            total = n = 0
            x = phase
            while x < w:
                xi = int(round(x))
                if 1 <= xi < w:
                    total += energy[xi]
                    n += 1
                x += p
            if n and (best_ph is None or total / n > best_ph[0]):
                best_ph = (total / n, phase)
        if best_ph:
            scored.append((p, best_ph[0], best_ph[1]))

    # Every MULTIPLE of the true period also lands only on grid lines, and
    # scores just as well on fewer samples -- so the maximum alone picks a
    # harmonic and resamples the sprite to half its real resolution. The
    # answer is the SMALLEST period that scores near the best one.
    top = max(s for _, s, _ in scored)
    for p, s, phase in scored:
        if s >= 0.95 * top:
            return p, phase
    return scored[0][0], scored[0][2]


def detect_phase(px, w, h, period, horizontal):
    """The grid offset along the other axis, given the period."""
    n_out = h if horizontal else w
    energy = [0] * n_out
    for i in range(1, n_out):
        s = 0
        rng = range(0, w, 3) if horizontal else range(0, h, 3)
        for j in rng:
            a = px[j, i - 1] if horizontal else px[i - 1, j]
            b = px[j, i] if horizontal else px[i, j]
            s += abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])
        energy[i] = s
    best = None
    for ph10 in range(0, int(period * 10)):
        phase = ph10 / 10.0
        total = n = 0
        v = phase
        while v < n_out:
            vi = int(round(v))
            if 1 <= vi < n_out:
                total += energy[vi]
                n += 1
            v += period
        if n and (best is None or total / n > best[0]):
            best = (total / n, phase)
    return best[1]


def median_sample(px, w, h, cx, cy, r=3):
    """The middle of one art pixel, as a median so JPEG ringing at the cell's
    edges cannot vote.

    `r` scales with the grid period: on a 1:1 image there is nothing to average
    and a 7x7 median would pull in the neighbouring art pixels instead."""
    if r <= 0:
        x, y = int(cx), int(cy)
        return px[min(max(x, 0), w - 1), min(max(y, 0), h - 1)][:3]
    cols = []
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            x, y = int(cx + dx), int(cy + dy)
            if 0 <= x < w and 0 <= y < h:
                cols.append(px[x, y])
    return tuple(sorted(c[i] for c in cols)[len(cols) // 2] for i in range(3))


def is_checker(c):
    return max(c) - min(c) < 20 and min(c) > 180


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--name", default="logo", help="C identifier prefix")
    ap.add_argument("--grid", default="auto",
                    help="'auto', or the grid period in pixels (may be fractional)")
    ap.add_argument("--phase", default=None,
                    help="grid offset as X,Y in pixels; measured when --grid is auto")
    ap.add_argument("--key-checker", action="store_true",
                    help="the chequerboard behind a flattened PNG is transparent")
    ap.add_argument("--max", type=int, default=128,
                    help="refuse a sprite wider or taller than this")
    args = ap.parse_args()

    src = Image.open(args.input)
    has_alpha = src.mode in ("RGBA", "LA") or "transparency" in src.info
    rgba = src.convert("RGBA")
    rgb = src.convert("RGB")
    w, h = rgb.size
    px = rgb.load()
    ap_ = rgba.load()

    if args.grid == "auto":
        period, phase_x = detect_period(px, w, h, 2, 40)
        phase_y = detect_phase(px, w, h, period, horizontal=True)
    else:
        period, phase_x, phase_y = float(args.grid), 0.0, 0.0
    if args.phase:
        phase_x, phase_y = (float(v) for v in args.phase.split(","))
    print("grid period %.2f px, phase %.1f,%.1f" % (period, phase_x, phase_y))
    sample_r = min(3, int(period / 4))

    nx = int((w - phase_x) // period)
    ny = int((h - phase_y) // period)

    cells, alpha = [], []
    for j in range(ny):
        crow, arow = [], []
        for i in range(nx):
            cx = phase_x + i * period + period / 2
            cy = phase_y + j * period + period / 2
            c = median_sample(px, w, h, cx, cy, sample_r)
            a = ap_[int(cx), int(cy)][3] if has_alpha else 255
            crow.append(c)
            arow.append(a >= 128)
        cells.append(crow)
        alpha.append(arow)

    if args.key_checker:
        # Flood fill from the border, so a pale pixel enclosed by the artwork
        # stays part of the artwork.
        seen = [[False] * nx for _ in range(ny)]
        q = deque()
        for i in range(nx):
            for j in (0, ny - 1):
                if is_checker(cells[j][i]):
                    seen[j][i] = True
                    q.append((i, j))
        for j in range(ny):
            for i in (0, nx - 1):
                if is_checker(cells[j][i]) and not seen[j][i]:
                    seen[j][i] = True
                    q.append((i, j))
        while q:
            i, j = q.popleft()
            for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                a, b = i + di, j + dj
                if 0 <= a < nx and 0 <= b < ny and not seen[b][a] and is_checker(cells[b][a]):
                    seen[b][a] = True
                    q.append((a, b))
        for j in range(ny):
            for i in range(nx):
                if seen[j][i]:
                    alpha[j][i] = False

    op = [(i, j) for j in range(ny) for i in range(nx) if alpha[j][i]]
    if not op:
        sys.exit("every pixel came out transparent; check --key-checker")
    x0 = min(i for i, _ in op); x1 = max(i for i, _ in op)
    y0 = min(j for _, j in op); y1 = max(j for _, j in op)
    ow, oh = x1 - x0 + 1, y1 - y0 + 1
    if ow > args.max or oh > args.max:
        sys.exit("sprite is %dx%d, over --max %d" % (ow, oh, args.max))
    print("sprite %dx%d" % (ow, oh))

    def rgb565(c):
        return ((c[0] & 0xF8) << 8) | ((c[1] & 0xFC) << 3) | (c[2] >> 3)

    stride = (ow + 7) // 8
    mask = bytearray(stride * oh)
    words = []
    opaque_n = 0
    for j in range(y0, y1 + 1):
        for i in range(x0, x1 + 1):
            words.append(rgb565(cells[j][i]) if alpha[j][i] else 0)
            if alpha[j][i]:
                opaque_n += 1
                u = i - x0
                mask[(j - y0) * stride + u // 8] |= 0x80 >> (u % 8)

    n = args.name
    guard = "QRSTYLE_LOGO_%s_H" % n.upper()
    out = []
    out.append("/*")
    out.append(" * %dx%d sprite for QrStyle_Logo, generated by tools/logo-to-c.py from" % (ow, oh))
    out.append(" * %s -- do not edit; re-run the script." % args.input.replace("\\", "/"))
    out.append(" *")
    out.append(" * %d bytes of RGB565 plus %d bytes of 1bpp alpha, both const." %
               (ow * oh * 2, stride * oh))
    out.append(" */")
    out.append("#ifndef %s" % guard)
    out.append("#define %s" % guard)
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("#define %s_LOGO_W %d" % (n.upper(), ow))
    out.append("#define %s_LOGO_H %d" % (n.upper(), oh))
    out.append("")
    out.append("static const uint16_t %s_logo_rgb565[%d] = {" % (n, ow * oh))
    for j in range(oh):
        row = words[j * ow:(j + 1) * ow]
        for k in range(0, ow, 10):
            out.append("    " + " ".join("0x%04X," % v for v in row[k:k + 10]))
    out.append("};")
    out.append("")
    out.append("static const uint8_t %s_logo_alpha[%d] = {" % (n, stride * oh))
    for j in range(oh):
        row = mask[j * stride:(j + 1) * stride]
        out.append("    " + " ".join("0x%02X," % v for v in row))
    out.append("};")
    out.append("")
    out.append("#endif /* %s */" % guard)

    with open(args.output, "w", newline="\n", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print("wrote %s -- %d opaque of %d pixels" % (args.output, opaque_n, ow * oh))


if __name__ == "__main__":
    main()
