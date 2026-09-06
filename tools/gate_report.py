#!/usr/bin/env python3
"""Per-boot defer-gate report from MERGELOG GATE lines (task #24, M3.192).

Per boot: gate counters at three moments (pre-sweep ~60s, post-sweep ~75s,
final), plus the channel-registry hex at ~70s for cross-boot diffing.
Counters are cumulative atomics — per-window deltas are what discriminate.

Usage: gate_report.py <mergelog> <summary>
"""
import re, sys

boots, cur = [], None
for ln in open(sys.argv[1]):
    m = re.match(r'PEEK t=(\d+)ms', ln)
    if m and int(m.group(1)) < 1000:
        cur = {'gates': []}
        boots.append(cur)
    if cur is None:
        continue
    m = re.match(r'GATE t=(\d+)ms bfe0=(\d+)/(\d+) c768=(\d+)/(\d+) sig=(\d+)'
                 r' v11=\S+(?: reg=([0-9A-F]+))?', ln)
    if m:
        cur['gates'].append((int(m.group(1)),) +
                            tuple(int(m.group(k)) for k in range(2, 7)) +
                            (m.group(7) or '',))
boots = [b for b in boots if b['gates']]
fams = []
for l in open(sys.argv[2]):
    m = re.match(r'\S+ MATCHED cam=\d+ draws=(\d+) wedge=(\d+)%', l)
    if m:
        fams.append(('M' if m.group(1) == '79' else 'W') + f"({m.group(1)}/{m.group(2)}%)")
print(f"boots={len(boots)} runs={fams}")


def at(gs, t):
    best = None
    for g in gs:
        if g[0] <= t:
            best = g
    return best or gs[0]


regs70 = []
for i, b in enumerate(boots):
    gs = b['gates']
    pre, post, fin = at(gs, 60000), at(gs, 76000), gs[-1]
    fam = fams[i] if i < len(fams) else '?'
    dt, db, dc, ds = (post[0] - pre[0], post[1] - pre[1],
                      post[3] - pre[3], post[5] - pre[5])
    print(f"boot{i + 1} {fam:12} sweepΔ({dt}ms): bfe0_true+{db} c768_true+{dc} "
          f"sig+{ds} | final bfe0={fin[1]}/{fin[2]} c768={fin[3]}/{fin[4]} sig={fin[5]}")
    regs70.append((fam, at(gs, 70000)[6]))
ref = regs70[0][1]
n = min(len(r) for _, r in regs70) if regs70 else 0
diff_offs = sorted({i // 2 for _, r in regs70 for i in range(0, n, 2)
                    if r[i:i + 2] != ref[i:i + 2]})
print(f"registry@70s byte-offsets differing across boots: {diff_offs[:40]}")
for fam, r in regs70:
    key = "".join(r[i * 2:i * 2 + 2] for i in diff_offs[:16])
    print(f"  {fam:12} diffbytes={key}")
