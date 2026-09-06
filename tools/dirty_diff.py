#!/usr/bin/env python3
"""Why does composition never stop on wedge boots? (task #24)

Reads M3.239 DIRTY lines (RESTUFF_DIRTYLOG=1) and answers the one open question.

The chain, all confirmed by grep on the recomp:

    sub_8297E410 runs the prune + dyntree + composer only while
        *(r3+4) != 0  AND  *(0x83341E9C) != 0  AND  *(*(r3+0)+732) != 0
    The +732 byte has exactly TWO writers:
        sub_829CC8C0  SET   (li r11,1 ; stb r11,732)   <- producer marks dirty
        sub_829CCB48  CLEAR (li r27,0 ; stb r27,732)   <- consumer marks clean
    and the CLEAR is conditional at its only call site, sub_82965290:
        if (*(r3+2484) == 0) skip;  if (*(r3+2464) == 0) skip;  clear();
    The two producers (sub_82963080, sub_82965B18) write BOTH gate fields and
    also call the SET, so this is a producer/consumer queue.

Clean boots stop composing at ~100s; wedge boots never do. Exactly three shapes
can cause that, and the counters separate them:

  A. PRODUCER NEVER STOPS   set keeps climbing after 100s on wedge boots while
                            it plateaus on clean ones. Root is upstream: find
                            what keeps submitting work.
  B. CONSUMER GATE CLOSED   clrseen climbs but clrtaken does NOT, i.e.
                            sub_82965290 runs but f2484/f2464 is NULL. Root is
                            whatever nulls that field.
  C. CONSUMER NOT CALLED    clrseen itself stops climbing. Root is upstream of
                            sub_82965290.

Usage:  tools/dirty_diff.py <mergelog> [--verdicts clean,wedge,...]
"""
import re
import sys

D = re.compile(
    r'DIRTY t=(\d+)ms obj=([0-9A-F]+) f4=(-?\d+) glob=(-?\d+) sub=([0-9A-F]+) '
    r'dirty732=(-?\d+) set=(\d+) clear=(\d+) clrseen=(\d+) clrtaken=(\d+) '
    r'f2484=([0-9A-F]+) f2464=([0-9A-F]+)')


def boots(path):
    out, cur, last = [], None, None
    for ln in open(path, errors='replace'):
        q = D.match(ln)
        if not q:
            continue
        t = int(q.group(1))
        if last is None or t < last - 5000:
            cur = []
            out.append(cur)
        last = t
        cur.append({
            't': t, 'dirty': int(q.group(6)), 'set': int(q.group(7)),
            'clear': int(q.group(8)), 'seen': int(q.group(9)),
            'taken': int(q.group(10)), 'f2484': q.group(11), 'f2464': q.group(12),
        })
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
                  f"t={b[0]['t']/1000:.0f}-{b[-1]['t']/1000:.0f}s")
            print(f"{'t(s)':>6} {'dirty':>6} {'set':>10} {'clear':>10} "
                  f"{'clrseen':>10} {'clrtaken':>10}  f2484/f2464")
            prev = None
            for r in b:
                # print every 20s plus every sample in the decisive 95-115s window
                if not (r['t'] % 20000 < 1100 or 95000 <= r['t'] <= 115000):
                    continue
                d = ''
                if prev:
                    d = (f"  (+{r['set']-prev['set']} set, "
                         f"+{r['clear']-prev['clear']} clr)")
                print(f"{r['t']/1000:6.0f} {r['dirty']:6d} {r['set']:10d} "
                      f"{r['clear']:10d} {r['seen']:10d} {r['taken']:10d}  "
                      f"{r['f2484']}/{r['f2464']}{d}")
                prev = r
            if len(b) >= 2:
                a, z = b[0], b[-1]
                late = [r for r in b if r['t'] >= 105000]
                if len(late) >= 2:
                    ds = late[-1]['set'] - late[0]['set']
                    dc = late[-1]['clear'] - late[0]['clear']
                    dseen = late[-1]['seen'] - late[0]['seen']
                    dtaken = late[-1]['taken'] - late[0]['taken']
                    print(f"   LATE (>=105s) deltas: set +{ds}  clear +{dc}  "
                          f"clrseen +{dseen}  clrtaken +{dtaken}")
                    if ds > 0 and dc > 0:
                        print("   => producer AND consumer both still running "
                              "(shape A: work keeps arriving)")
                    elif ds > 0 and dc == 0 and dseen > 0 and dtaken == 0:
                        print("   => shape B: CONSUMER GATE CLOSED "
                              "(f2484/f2464 null) -- root is whoever nulls it")
                    elif ds > 0 and dseen == 0:
                        print("   => shape C: consumer NOT CALLED at all")
                    elif ds == 0:
                        print("   => producer stopped (expected on CLEAN boots)")


if __name__ == '__main__':
    main()
