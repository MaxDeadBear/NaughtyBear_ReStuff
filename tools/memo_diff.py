#!/usr/bin/env python3
"""Which half of the memoisation check fails on wedge boots? (task #24)

Reads M3.241 MEMO lines (RESTUFF_MEMOLOG=1).

The site: sub_82AD8AB0 (recomp.51.cpp:2714) runs the light-list rebuild
sub_82B0CBC8 UNCONDITIONALLY, then

    if (sub_82B0A7D0(...) == 0) skip;
    same = memcmp(node+64, cur+0, 32) == 0;            # four 64-bit words
    if (same && *(node+104) == *(0x836E7E74)) skip;    # generation match
    else compose;                                      # -> sub_82AD3050

and only a MISS reaches the composer. This is the one model that explains the
whole observation: MASK# is logged inside the rebuild (BEFORE the check) and
keeps flowing to 249s on clean boots, while LL comes from the composer (AFTER
it) and stops dead at ~100s on clean boots but never on wedge boots.

⛔ RESULT (memo1, Aug 11): THE MEMO IS NOT THE DISCRIMINATOR. memo11 (WEDGE
285dr) and memo12 (CLEAN 79dr), both with excellent cameras, gave IDENTICAL
counters:

    memo11 WEDGE: calls +32550  same 100.0%  genmatch 0.0%  compose +32550
    memo12 CLEAN: calls +32536  same 100.0%  genmatch 0.0%  compose +32536

So "shape B / the generation never matches" is a UNIVERSAL property of our build
(the global at 0x836E7E74 is uninitialised .bss and nothing ever writes it), true
on clean boots too. Everything at sub_82AD8AB0 -- gate, 32-byte block compare,
generation test, compose decision, call rate -- is the same on both boot kinds.

=> The divergence is strictly BELOW it, inside sub_82AD3460's recursive walk,
which is the sole live caller of the composer (every LL row carries lr=82AD350C).
M3.243 therefore adds rec=/comp= (entries into sub_82AD3460 vs actual
sub_82AD3050 calls). Equal rec with different comp => the gate INSIDE the
recursion splits the boots; different rec => the walk itself does.

Usage:  tools/memo_diff.py <mergelog> [--verdicts clean,wedge,...]
"""
import re
import sys

# gate=/gatepass= count sub_82B0A7D0, the test that must pass before the memo
# check is even reached -- a third way composition could stop. They were added
# after the first build, so keep them OPTIONAL here and both log formats parse.
# (A stale regex silently matching nothing has already cost this project a
# session once; the groups are numbered so 8/9 simply come back None.)
M = re.compile(r'MEMO t=(\d+)ms calls=(\d+) same=(\d+) genmatch=(\d+) '
               r'compose=(\d+) gennow=([0-9A-F]+) gennode=([0-9A-F]+)'
               r'(?: gate=(\d+) gatepass=(\d+))?'
               r'(?: rec=(\d+) comp=(\d+))?')


def boots(path):
    out, cur, last = [], None, None
    for ln in open(path, errors='replace'):
        q = M.match(ln)
        if not q:
            continue
        t = int(q.group(1))
        if last is None or t < last - 5000:
            cur = []
            out.append(cur)
        last = t
        cur.append(tuple(int(q.group(i)) for i in range(1, 6)) +
                   (q.group(6), q.group(7),
                    int(q.group(8)) if q.group(8) else 0,
                    int(q.group(9)) if q.group(9) else 0,
                    int(q.group(10)) if q.group(10) else 0,
                    int(q.group(11)) if q.group(11) else 0))
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    verdicts = []
    for i, a in enumerate(sys.argv):
        if a == '--verdicts' and i + 1 < len(sys.argv):
            verdicts = sys.argv[i + 1].split(',')
    if not args:
        sys.exit(__doc__)

    for path in args:
        print(f"\n########## {path}")
        for bi, b in enumerate(boots(path)):
            tag = verdicts[bi] if bi < len(verdicts) else '?'
            print(f"\n===== boot{bi+1} [{tag}]  {len(b)} samples "
                  f"t={b[0][0]/1000:.0f}-{b[-1][0]/1000:.0f}s")
            print(f"{'t(s)':>6} {'d_calls':>9} {'d_same':>8} {'d_gen':>8} "
                  f"{'d_compose':>10}  {'same%':>6} {'gen%':>6}  gennow/gennode")
            prev = None
            for r in b:
                t, calls, same, gen, comp, gnow, gnode, gate, gpass, rec, ccomp = r
                if prev and not (t % 10000 < 1100 or 95000 <= t <= 115000):
                    continue
                if prev:
                    dc = calls - prev[1]
                    ds = same - prev[2]
                    dg = gen - prev[3]
                    dk = comp - prev[4]
                    if dc:
                        print(f"{t/1000:6.0f} {dc:9d} {ds:8d} {dg:8d} {dk:10d}  "
                              f"{100*ds/dc:5.1f}% {100*dg/dc:5.1f}%  {gnow}/{gnode}")
                prev = r
            # verdict for the late window
            late = [r for r in b if r[0] >= 105000]
            if len(late) >= 2:
                dc = late[-1][1] - late[0][1]
                ds = late[-1][2] - late[0][2]
                dg = late[-1][3] - late[0][3]
                dk = late[-1][4] - late[0][4]
                if dc:
                    drec = late[-1][9] - late[0][9]
                    dcomp = late[-1][10] - late[0][10]
                    print(f"   LATE(>=105s): calls +{dc}  same {100*ds/dc:.1f}%  "
                          f"genmatch {100*dg/dc:.1f}%  compose +{dk}")
                    if drec or dcomp:
                        # M3.243: this is the live question -- equal rec with
                        # different comp means the gate INSIDE sub_82AD3460
                        # splits the boots; different rec means the walk does.
                        print(f"   LATE recursion: sub_82AD3460 +{drec}  "
                              f"composer sub_82AD3050 +{dcomp}  "
                              f"comp/rec={dcomp/max(drec,1):.2f}")
                    if dk == 0:
                        print("   => memo HITS: composer never runs (expect CLEAN)")
                    elif ds * 100 < dc * 50:
                        print("   => shape A: BLOCK CHURN -- the 32-byte state keeps changing")
                    elif dg * 100 < dc * 50:
                        print("   => shape B: GENERATION never matches "
                              "(block settles, stamp does not)")
                    else:
                        print("   => misses despite both looking high; inspect per-node")


if __name__ == '__main__':
    main()
