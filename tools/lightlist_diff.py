#!/usr/bin/env python3
"""Light-list filter inputs, wedge vs clean (task #24).

Reads LL# lines (RESTUFF_LISTLOG, M3.223): per composer call, the source
light list, the dest (EXCLUSION) list, and each source light's 16-byte
sphere vector (BE floats: cx, cy, cz, radius).

Tests the ordering hypothesis directly: light 2 sits at (-27.1, 11.5, 18.7)
r=12 — over 100 units from the door at x~81.8 — so EXCLUDING it there is the
geometrically correct answer. The anomaly is clean boots failing to exclude
it, which points at composition running BEFORE the lights are registered
(empty source list -> empty exclusion set -> keys match -> merge). If so:
  - clean boots' door-window calls show EMPTY/short source lists
  - wedge boots' show light 2 present and excluded
  - the VECTORS are identical across families (data race refuted)

Usage: lightlist_diff.py <mergelog> [...]
"""
import re
import struct
import sys
from collections import Counter

# NOTE: M3.230 added the srcp= field; it is OPTIONAL here so this tool reads
# both pre- and post-M3.230 logs. A regex without it silently matches NOTHING.
LL = re.compile(r'LL#(\d+) t=(\d+)ms key=([0-9A-F]+) (?:srcp=[0-9A-F]+ )?(?:r4=[0-9A-F]+ )?(?:lr=[0-9A-F]+ )?src=\[([\d,]*)\] dst=\[([\d,]*)\](.*)')
LV = re.compile(r'L(\d+)=([0-9A-F]{32})')


def fvec(h):
    return struct.unpack('>4f', bytes.fromhex(h))


def boots(path):
    out, cur, last = [], None, None
    for ln in open(path, errors='replace'):
        m = re.match(r'(?:LL#\d+|SCHED|STAB|PEEK|MERGER#\d+) t=(\d+)ms', ln)
        if m:
            t = int(m.group(1))
            if last is None or t < last - 5000:
                cur = []
                out.append(cur)
            last = t
        if cur is None:
            continue
        q = LL.match(ln)
        if q:
            src = [int(x) for x in q.group(4).split(',') if x]
            dst = [int(x) for x in q.group(5).split(',') if x]
            vecs = {int(a): b for a, b in LV.findall(q.group(6))}
            cur.append((int(q.group(2)), q.group(3), src, dst, vecs))
    return out


for path in sys.argv[1:]:
    print(f"\n########## {path}")
    all_vecs = {}
    for bi, b in enumerate(boots(path), 1):
        if not b:
            print(f"  boot{bi}: no LL data")
            continue
        early = [x for x in b if x[0] < 100000]
        late = [x for x in b if x[0] >= 100000]
        print(f"  boot{bi}: calls={len(b)}  early(<100s)={len(early)} late={len(late)}")
        for tag, rows in (('early', early), ('late', late)):
            if not rows:
                continue
            srclens = Counter(len(r[2]) for r in rows)
            with2 = [r for r in rows if 2 in r[2]]
            excl2 = [r for r in rows if 2 in r[3]]
            print(f"      {tag}: src-len dist={dict(sorted(srclens.items()))}  "
                  f"src-has-2={len(with2)}  dst-excludes-2={len(excl2)}")
        # collect vectors for cross-boot identity check
        for _, _, _, _, vecs in b:
            for idx, h in vecs.items():
                all_vecs.setdefault(idx, Counter())[h] += 1
    print("\n  === light vectors: distinct values per index across ALL boots ===")
    for idx in sorted(all_vecs):
        c = all_vecs[idx]
        top = c.most_common(1)[0]
        mark = "" if len(c) == 1 else f"  <== {len(c)} DISTINCT VALUES"
        x, y, z, r = fvec(top[0])
        print(f"    L{idx}: ({x:8.2f},{y:7.2f},{z:8.2f}) r={r:6.2f}  x{top[1]}{mark}")
        if len(c) > 1:
            for h, n in c.most_common(4):
                x, y, z, r = fvec(h)
                print(f"         variant x{n}: ({x:.2f},{y:.2f},{z:.2f}) r={r:.2f}")
