#!/usr/bin/env python3
"""Find see-through holes in ground geometry: backdrop-coloured pockets ENCLOSED by world.

    tools/find_ground_holes.py <drive_dir> [--min N]

Why this is topological and not a colour/band test
--------------------------------------------------
A hole shows the backdrop through missing ground, so it is exactly the SAME
colour as the legitimate distant water/sky (measured: hole (55,90,122) vs known
water (55,80,95)..(69,104,125)). No colour threshold or y-band can separate
them, and two earlier versions of this tool got it wrong in both directions:

  - v1 scanned only the bottom 25%. A hole at y~480-600 fell mostly outside the
    band and reported 2px instead of ~1000 -- and I wrote that up as a
    root-cause "fix" (the alpha test) that was nothing of the sort. The user
    spotted the hole still in the frame I had called clean.
  - v2 widened to y>400 and matched the hole colour exactly. That flagged 121
    frames, most of them legitimate water visible low in frame whenever the
    camera looks down a slope.

The real discriminator is CONNECTIVITY: sky and distant water reach the frame
border; a hole is an ISOLATED pocket of backdrop colour with world geometry all
around it. So: threshold for backdrop colour, label connected components,
discard every component touching the border, report what remains.

⚠️⚠️ AUTOMATED DETECTION IS NOT RELIABLE HERE -- THIS TOOL RANKS CANDIDATES, IT
DOES NOT MEASURE THE DEFECT. Three generations all over- or under-counted:
band-only (missed a 1000px hole entirely), colour+band (flagged 121 frames of
legitimate low water), and enclosure+foliage-rejection (still 160-335k px per
run, because sky seen between BROWN TREE TRUNKS is enclosed by non-green
geometry and is topologically identical to a ground hole). Do NOT use the totals
to A/B two runs -- I did, and it produced a retracted root cause.
The only trustworthy check is looking at the frame, which is how the user found
the hole I had declared fixed.

⚠️ Still eyeball every hit. A legitimately enclosed patch of backdrop -- seen
through a window, or a gap between rocks -- is topologically identical to a
hole. This narrows candidates; it does not certify them.
"""
import sys, glob, os
from collections import deque
from PIL import Image

def is_backdrop(p):
    """Sky / distant water, measured from real frames."""
    r, g, b = p
    return abs(r - 60) < 30 and abs(g - 92) < 32 and abs(b - 118) < 38

def find_pockets(path, step=2, min_cells=40):
    im = Image.open(path).convert('RGB')
    w, h = im.size
    px = im.load()
    W, H = w // step, h // step
    mask = [[is_backdrop(px[x * step, y * step]) for x in range(W)] for y in range(H)]
    seen = [[False] * W for _ in range(H)]
    pockets = []
    for sy in range(H):
        for sx in range(W):
            if not mask[sy][sx] or seen[sy][sx]:
                continue
            q = deque([(sx, sy)])
            seen[sy][sx] = True
            comp = []
            touches_border = False
            while q:
                x, y = q.popleft()
                comp.append((x, y))
                if x == 0 or y == 0 or x == W - 1 or y == H - 1:
                    touches_border = True
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < W and 0 <= ny < H and mask[ny][nx] and not seen[ny][nx]:
                        seen[ny][nx] = True
                        q.append((nx, ny))
            if not touches_border and len(comp) >= min_cells:
                # Enclosure alone is not enough: FOLIAGE encloses sky constantly
                # (gaps between leaves), which is why the purely topological
                # version reported ~347k px per run. Classify by what RINGS the
                # pocket -- a ground hole is bordered by path/dirt/stone, a leaf
                # gap by green. Sample the ring just outside the component.
                cells = set(comp)
                ring = []
                for (x, y) in comp:
                    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        nx, ny = x + dx, y + dy
                        if (nx, ny) not in cells and 0 <= nx < W and 0 <= ny < H:
                            ring.append(px[nx * step, ny * step])
                if not ring:
                    continue
                green = sum(1 for r, g, b in ring if g > r * 1.15 and g > b * 1.15)
                if green > len(ring) * 0.35:
                    continue  # leaf/foliage gap, not a ground hole
                xs = [c[0] * step for c in comp]
                ys = [c[1] * step for c in comp]
                pockets.append((len(comp) * step * step, (min(xs), min(ys)), (max(xs), max(ys))))
    pockets.sort(reverse=True)
    return pockets

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    d = sys.argv[1]
    min_cells = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[2] == '--min' else 40
    total = 0
    for f in sorted(glob.glob(os.path.join(d, 'W*.png'))):
        for area, lo, hi in find_pockets(f, min_cells=min_cells):
            print(f'{os.path.basename(f)}: ENCLOSED pocket ~{area}px bbox={lo}-{hi}')
            total += area
    print(f'TOTAL enclosed backdrop area: {total}px')
    return 0

if __name__ == '__main__':
    sys.exit(main())
