#!/usr/bin/env python3
"""Compare the MERGER's inputs across boots (task #24).

The merger coalesces adjacent runs only while their 32-byte keys stay EQUAL
and the combined index total fits 0xFFFF (decompile). mrg37 showed call
VOLUME is family-independent while the resulting table differs threefold, so
the fork has to be in these inputs — this reports both predicates directly.

MKEY/MREC come from RESTUFF_MKEY=<t0,t1>; run it over the 65-70s burst, which
is when the merge actually happens (everything after ~84s is frozen).

Record layout (decoded from live dumps): +0..11 bbox min, +16..27 bbox max,
+32 owner ptr, +36 key ptr, +44 and +48 two counts (index/vertex class).

Usage: mkey_diff.py <mergelog> [...]
"""
import re
import sys
from collections import Counter

MKEY = re.compile(r'MKEY#(\d+) t=(\d+)ms r3=([0-9A-F]+) obj=([0-9A-F]+) recp=([0-9A-F]+) n=(\d+)')
MREC = re.compile(r'MREC#(\d+)\.(\d+) rec=([0-9A-F]+) keyp=([0-9A-F]+) key=([0-9A-F]+)')


def be(hexstr, off, n=4):
    return int(hexstr[2 * off:2 * (off + n)], 16)


def boots(path):
    out, cur = [], None
    for ln in open(path, errors='replace'):
        if ln.startswith('PEEK t='):
            m = re.search(r'PEEK t=(\d+)ms', ln)
            if m and int(m.group(1)) < 20000:
                cur = {'calls': {}, 'recs': {}}
                out.append(cur)
        elif cur is None:
            continue
        elif ln.startswith('MKEY#'):
            m = MKEY.match(ln)
            if m:
                cur['calls'][int(m.group(1))] = (int(m.group(2)), int(m.group(6)))
        elif ln.startswith('MREC#'):
            m = MREC.match(ln)
            if m:
                cur['recs'].setdefault(int(m.group(1)), []).append(
                    (int(m.group(2)), m.group(3), m.group(4), m.group(5)))
    return out


for path in sys.argv[1:]:
    print(f"\n########## {path}")
    for bi, b in enumerate(boots(path), 1):
        if not b['recs']:
            print(f"  boot{bi}: no MKEY data")
            continue
        # Records past the live prefix are EF-filled (uninitialised marker) and
        # some carry an FFFFFFFF sentinel at +44; counting either would corrupt
        # the 0xFFFF-limit metric, which is one of the two coalesce predicates.
        def valid(rec):
            if len(rec) < 2 * 52:
                return False
            n = be(rec, 44)
            return n != 0xFFFFFFFF and n != 0xEFEFEFEF and rec[:8] != 'EFEFEFEF'

        keys = Counter()
        eq_adj = neq_adj = 0
        over = 0
        counts = Counter()
        nrecs = Counter()
        skipped = 0
        for call, raw in b['recs'].items():
            raw.sort()
            recs = [r for r in raw if valid(r[1])]
            skipped += len(raw) - len(recs)
            for _, rec, _keyp, key in recs:
                keys[key] += 1
                counts[be(rec, 44)] += 1
            for a, c in zip(recs, recs[1:]):
                if a[3] == c[3]:
                    eq_adj += 1
                else:
                    neq_adj += 1
                if be(a[1], 44) + be(c[1], 44) > 0xFFFF:
                    over += 1
            nrecs[b['calls'].get(call, (0, 0))[1]] += 1
        tot_adj = eq_adj + neq_adj
        print(f"  boot{bi}: calls={len(b['calls'])} recs={sum(len(v) for v in b['recs'].values())}")
        print(f"      adjacent pairs: key-EQUAL={eq_adj} key-DIFFER={neq_adj}"
              f"  ({100 * eq_adj / max(tot_adj, 1):.1f}% equal)")
        print(f"      pairs whose combined count exceeds 0xFFFF: {over}")
        print(f"      distinct keys={len(keys)}  top: "
              + ", ".join(f"{k[:20]}..x{v}" for k, v in keys.most_common(3)))
        print(f"      record counts(+44) distinct={len(counts)} "
              f"max={max(counts) if counts else 0} top={counts.most_common(3)}")
        print(f"      array sizes n= {nrecs.most_common(4)}   skipped(invalid recs)={skipped}")
