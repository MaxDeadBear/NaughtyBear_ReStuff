#!/usr/bin/env python3
"""Whole-frame water-blue region detector (task #24).

Replaces the fixed score box, which silently missed relocated wedges on
drifted runs (a triangle sat in a frame scored 0% — user-caught) and scored
black frames as clean. Reports every blue region outside HUD zones and saves
a crop per region for HUMAN review (the user judges stills; no boot needed).

Usage: wedge_regions.py <frame.png> <out_prefix>
Prints: REGIONS <n> <x0,y0,x1,y1,npts>;...
"""
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert('RGB')
prefix = sys.argv[2]
px = im.load()
W, H = im.size
pts = []
for y in range(40, H - 40, 2):
    for x in range(0, W, 2):
        if x < 300 and y < 140: continue          # score HUD
        if 500 < x < 780 and y < 110: continue    # combo HUD
        if x < 340 and y > 600: continue          # objective text
        if x > 1000 and y > 470: continue         # minimap
        r, g, b = px[x, y]
        if b > r + 18 and 40 < b < 200 and g > r - 10 and g < b + 30:
            pts.append((x, y))
clusters = []
for p in pts:
    for c in clusters:
        if abs(p[0] - c[-1][0]) < 40 and abs(p[1] - c[-1][1]) < 40:
            c.append(p); break
    else:
        clusters.append([p])
out = []
for cp in clusters:
    if len(cp) < 12: continue
    xs = [q[0] for q in cp]; ys = [q[1] for q in cp]
    out.append((min(xs), min(ys), max(xs), max(ys), len(cp)))
out.sort(key=lambda r: -r[4])
print("REGIONS", len(out), ";".join(f"{a},{b},{c},{d},{n}" for a, b, c, d, n in out[:8]))
for i, (x0, y0, x1, y1, n) in enumerate(out[:4]):
    im.crop((max(0, x0 - 30), max(0, y0 - 30), min(W, x1 + 30), min(H, y1 + 30))).save(
        f"{prefix}_r{i}.png")
