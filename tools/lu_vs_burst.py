#!/usr/bin/env python3
"""Does the merge burst race the LU streamer? (task #24)

The merge burst (~65-70s) is a CONSUMER of whatever the LU streamer has
produced, and post-load re-dirtying does not heal a wedge boot, so the load-era
ordering is worth measuring. So the mechanism should be visible as an ordering: on a
boot that ends up merged, more units should have landed before the burst than
on one that ends up wedged.

Needs M3.215, which mirrors LU transitions into the mergelog on the ARM clock —
before that the streaming timeline (REXLOG wall clock, shed by rotation) and
the merge timeline (arm-relative ms) could not be compared at all.

⚠️ The signature printed alongside is NOT the outcome — mk1 refuted that. Pair
this with the real verdict (draws/wedge% at the door) from wedge_ab.

Usage: lu_vs_burst.py <mergelog> [...]
"""
import re
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from sector_signature import signature  # noqa: E402

FRONTEND_CTX = 'A660C7E0'


def boots(path):
    out, cur, r3 = [], None, None
    for ln in open(path, errors='replace'):
        if ln.startswith('PEEK t='):
            m = re.search(r'PEEK t=(\d+)ms', ln)
            if m and int(m.group(1)) < 20000:
                cur = {'lu': [], 'merger': [], 'stab': []}
                out.append(cur)
        elif cur is None:
            continue
        elif ln.startswith('LU t='):
            m = re.match(r'LU t=(\d+)ms (\S+) lu=([0-9A-F]+) state=(\d+) name=(.*)', ln)
            if m:
                cur['lu'].append((int(m.group(1)), m.group(2), m.group(3),
                                  int(m.group(4)), m.group(5).strip()))
        elif ln.startswith('MERGER#'):
            m = re.match(r'MERGER#\d+ t=(\d+)ms', ln)
            if m:
                cur['merger'].append(int(m.group(1)))
        elif ln.startswith('SCHED '):
            m = re.match(r'SCHED t=\d+ms bits=[0-9A-F]+ r3=([0-9A-F]+)', ln)
            if m:
                r3 = m.group(1)
        elif ln.startswith('STAB '):
            m = re.match(r'STAB t=(\d+)ms tab=[0-9A-F]+ d=([0-9A-F]+)', ln)
            if m and r3 and r3 != FRONTEND_CTX:
                cur['stab'].append((int(m.group(1)), m.group(2)))
    return out


for path in sys.argv[1:]:
    print(f"\n########## {path}")
    for i, b in enumerate(boots(path), 1):
        if not b['merger']:
            print(f"  boot{i}: no merger data")
            continue
        burst0 = min(b['merger'])
        # burst end = last call before the rate collapses to the idle cadence
        sig = "?"
        if b['stab']:
            sig = "/".join(str(v) for v in signature(b['stab'][0][1])[0])
        lu = b['lu']
        done = [t for t, _st, _u, state, _n in lu if state >= 6]
        print(f"  boot{i}: signature={sig}  burst_start={burst0}ms  LU events={len(lu)}")
        if lu:
            print(f"      LU span {min(t for t, *_ in lu)}..{max(t for t, *_ in lu)}ms"
                  f"   completions(state>=6)={len(done)}")
            if done:
                before = sum(1 for t in done if t < burst0)
                print(f"      completions BEFORE burst={before} / {len(done)}"
                      f"   last completion={max(done)}ms"
                      f"   ({'BEFORE' if max(done) < burst0 else 'AFTER'} burst start)")
            names = {}
            for t, _st, _u, state, n in lu:
                names.setdefault(n, []).append((t, state))
            late = [(max(t for t, _ in v), n) for n, v in names.items()
                    if max(t for t, _ in v) > burst0]
            late.sort(reverse=True)
            if late:
                print(f"      units still transitioning AFTER burst start: {len(late)}"
                      f"  latest: {[(n, t) for t, n in late[:5]]}")
        else:
            print("      (no LU lines — needs RESTUFF_LULOG=1 with M3.215)")
