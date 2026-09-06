#!/usr/bin/env python3
"""Which light survives the collapse — clean boots vs wedge boots (task #24).

Background (Aug 11). The composer's source light list universally collapses to a
SINGLE light by t=100-105s; that collapse is NOT the wedge discriminator (it
happens identically on clean boots). What differs is the merge-key byte 23
histogram over the record array at that point:

    clean boot (keyhist14, 79 draws) -> UNIFORM, every key 00
    wedge boot (keyhist15, 281 draws) -> two-way split, 00:76 04:87

It was built to test WHICH PHYSICAL LIGHT survives at that ordinal (a wide light
would reach every record -> nothing excluded -> merge; a narrow one would not).

⛔ THAT HYPOTHESIS IS REFUTED. Run against the archived ll1 batch (ll11 clean,
ll12 clean, ll14 WEDGE) every boot shows the IDENTICAL survivor:

    boot1 clean : L2 (88.60, 8.31, -53.77) radius=8.00
    boot2 clean : L2 (88.60, 8.31, -53.77) radius=8.00
    boot3 WEDGE : L2 (88.60, 8.31, -53.77) radius=8.00

Same light, same radius, same [2] source list. The survivor is not the variable.

✅ WHAT THE SAME DATA DID SHOW — the real discriminator is TIMING, not identity.
All three boots compose identically from 60-100s (442/439/433 samples per 10s,
falling to 171/172/173), and then at t~100s they split cleanly:

    clean boots : composition STOPS DEAD (last LL 100.2s / 100.3s, though the
                  process runs to 260s, and the 1-in-997 sampler never stops --
                  so zero rows means zero calls)
    wedge boot  : keeps composing at a steady ~145 samples/10s through 200s

and it is not one stuck node: the wedge boot recomposes 482 DISTINCT keys late,
~half taking the light-2 exclusion (dst '' 761 / '2,' 613) -- precisely the
76/87 byte-23 split KEYHIST reports. So composition CONTINUING past ~100s is
what bakes the single-light exclusion into half the records and splits the keys.

OPEN QUESTION this script no longer answers: who keeps driving the pass after
100s? M3.238 adds `r4=` (the owner) and `lr=` (the caller) to every LL line for
exactly that; run a KEYFIX-OFF batch with
    RESTUFF_LISTLOG=1 RESTUFF_KEYHIST=1 RESTUFF_IBWATCH=13F00000,13F01000
and compare the late-window lr= histogram against the early one.

⚠️ Do NOT compare raw LL row COUNTS between boots — the 1-in-997 sampler returns
more rows from boots that make more composer calls (a wedge boot makes ~2.9x
more). Compare per-time-bucket RATES, and spheres, not counts.
"""
import re
import struct
import sys
from collections import Counter

# M3.238 added r4= and lr=; keep them NON-capturing so group numbers below
# stay valid, and OPTIONAL so pre-M3.238 logs still parse. (A stale regex
# silently matching nothing already cost a session once today.)
LL = re.compile(r'LL#(\d+) t=(\d+)ms key=([0-9A-F]+) (?:srcp=([0-9A-F]+) )?'
                r'(?:r4=[0-9A-F]+ )?(?:lr=[0-9A-F]+ )?'
                r'src=\[([\d,]*)\] dst=\[([\d,]*)\](.*)')
LV = re.compile(r'L(\d+)=([0-9A-F]{32})')
KH = re.compile(r'KEYHIST t=(\d+)ms nrec=(\d+) scanned=(\d+)(.*)')
ANY = re.compile(r'(?:KEYHIST|LL#\d+|MASK#\d+|MERGER#\d+|SCHED|STAB|QKEY) t=(\d+)ms')

LATE = 100_000   # ms; the collapse completes at 100-105s


def sphere(h):
    cx, cy, cz, r = struct.unpack('>4f', bytes.fromhex(h))
    return cx, cy, cz, r


def load(path):
    boots, cur, last = [], None, None
    for ln in open(path, errors='replace'):
        a = ANY.match(ln)
        if a:
            t = int(a.group(1))
            if last is None or t < last - 5000:
                cur = {'ll': [], 'kh': []}
                boots.append(cur)
            last = t
        if cur is None:
            continue
        q = LL.match(ln)
        if q:
            cur['ll'].append((int(q.group(2)),
                              [int(x) for x in q.group(5).split(',') if x],
                              [int(x) for x in q.group(6).split(',') if x],
                              {int(a): b for a, b in LV.findall(q.group(7))}))
            continue
        q = KH.match(ln)
        if q:
            vals = {}
            for kv in q.group(4).split():
                k, v = kv.split(':')
                vals[k] = int(v)
            cur['kh'].append((int(q.group(1)), int(q.group(2)), vals))
    return boots


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    verdicts = []
    for i, a in enumerate(sys.argv[1:]):
        if a == '--boot-verdicts':
            verdicts = sys.argv[i + 2].split(',')
    if not args:
        sys.exit(__doc__)

    for path in args:
        print(f"\n########## {path}")
        for bi, b in enumerate(load(path)):
            tag = verdicts[bi] if bi < len(verdicts) else '?'
            print(f"\n===== boot{bi + 1}  [{tag}]")
            if b['kh']:
                t, nrec, vals = b['kh'][-1]
                uni = sum(1 for _, _, v in b['kh'] if len(v) == 1)
                print(f"   KEYHIST: {len(b['kh'])} samples, uniform={uni}, "
                      f"final t={t/1000:.0f}s nrec={nrec} {vals}")
                print(f"   => {'MERGED/uniform (expect CLEAN)' if len(vals) == 1 else 'SPLIT (expect WEDGE)'}")
            late = [r for r in b['ll'] if r[0] >= LATE]
            if not late:
                print("   no late LL rows (composer stopped — typical of clean boots)")
                continue
            srcs = Counter(tuple(r[1]) for r in late)
            print(f"   late LL rows={len(late)}  source lists: {dict(srcs.most_common(4))}")
            # the surviving light's sphere
            seen = {}
            for _, src, _dst, vecs in late:
                for k in src:
                    if k in vecs:
                        seen.setdefault(k, Counter())[vecs[k]] += 1
            for k in sorted(seen):
                for h, n in seen[k].most_common(3):
                    cx, cy, cz, r = sphere(h)
                    print(f"      L{k}: ({cx:8.2f},{cy:7.2f},{cz:8.2f})  radius={r:6.2f}  x{n}")


if __name__ == '__main__':
    main()
