#!/usr/bin/env python3
"""Classify a captured boot frame: DIALOG / MENU / GAMEPLAY / OTHER.

WHY THIS EXISTS: wedge_ab.sh lumps every non-gameplay frame into
"NOT GAMEPLAY -- menu/dialog frame", but those are two completely different
outcomes. A MENU frame means the replay simply did not advance (a timing miss,
harmless). A DIALOG frame means the sign-in prompt appeared and the boot is
stuck on it -- the actual defect. Counting them together makes the sign-in
incidence unmeasurable, and eyeballing 24 runs by hand does not scale.

ORDER MATTERS: test RED FIRST. The main menu has a big red panel AND a pale
blue sky, so a blue-first test calls the menu a dialog every time (it did).

  usage: classify_boot.py <dir-with-env_*-subdirs> [prefix]
"""
import os
import sys
from collections import Counter

try:
    from PIL import Image
except ImportError:
    sys.exit("needs pillow: pip install pillow")


def classify(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    im = im.resize((w // 4, h // 4))
    w, h = im.size
    px = im.load()
    red = blue = total = 0
    # Sample the right half / middle band, where both the menu panel and the
    # dialog box live; skip the HUD corners.
    for y in range(int(h * 0.15), int(h * 0.85)):
        for x in range(int(w * 0.15), int(w * 0.95)):
            r, g, b = px[x, y]
            total += 1
            if r > 140 and r > g + 45 and r > b + 45:
                red += 1
            elif b > 150 and b > r + 25 and g > 150 and abs(int(g) - int(b)) < 60:
                blue += 1
    if not total:
        return "OTHER", 0.0, 0.0
    rp, bp = 100.0 * red / total, 100.0 * blue / total
    if rp >= 4.0:                 # RED FIRST -- the menu also has a blue sky
        return "MENU", rp, bp
    if bp >= 18.0:                # the pale sign-in box dominates the frame
        return "DIALOG", rp, bp
    return "GAMEPLAY", rp, bp


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "/tmp/restuff_drive"
    prefix = sys.argv[2] if len(sys.argv) > 2 else "env_"
    counts = Counter()
    for name in sorted(os.listdir(root)):
        if not name.startswith(prefix):
            continue
        frame = os.path.join(root, name, "W01.png")
        if not os.path.exists(frame):
            continue
        kind, rp, bp = classify(frame)
        counts[kind] += 1
        print("%-24s %-9s red=%5.2f%% blue=%5.2f%%" % (name, kind, rp, bp))
    print("\n" + " ".join("%s=%d" % (k, v) for k, v in sorted(counts.items())))


if __name__ == "__main__":
    main()
