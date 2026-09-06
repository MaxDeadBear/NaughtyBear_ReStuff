#!/usr/bin/env python3
"""Join MERGELOG + IBWATCH side files with a wedge_ab summary (task #24).

Both side files accumulate across a batch's boots. A boot's segment starts at
its baseline "PEEK t=<small>ms" (mergelog) / "hit#0 " (ibwatch) line; only
segments containing a late-arm record belong to boots that survived to the
armed window, and those map IN ORDER onto the batch's summary lines (failed
pre-intro attempts write nothing past t~22s). Counts are checked and any
mismatch is flagged loudly instead of silently misaligning.

Usage: mergelog_join.py <mergelog> <ibwatch> <summary>
Per completed boot: family verdict | SCHED passes | MERGER calls (+unique
sector contexts r4, top repeats) | 1B watch hits + distinct writer offsets.
"""
import re, sys


def segments(path, is_start, keep):
    segs, cur = [], None
    try:
        f = open(path)
    except OSError:
        return segs
    for ln in f:
        if is_start(ln):
            cur = {'lines': []}
            segs.append(cur)
        if cur is not None and keep(ln):
            cur['lines'].append(ln.rstrip())
    return segs


def main():
    mlog, iwatch, summary = sys.argv[1:4]
    msegs = segments(
        mlog,
        lambda l: (m := re.match(r'PEEK t=(\d+)ms', l)) and int(m.group(1)) < 1000,
        lambda l: True)
    isegs = segments(iwatch, lambda l: l.startswith('hit#0 '), lambda l: True)
    marmed = [s for s in msegs if any(l.startswith('SCHED') and
              int(re.search(r't=(\d+)ms', l).group(1)) > 110000 for l in s['lines'])
              or any('late-arm' in l for l in s['lines'])]
    # mergelog has no explicit arm line; ibwatch does. Completed = armed.
    iarmed = [s for s in isegs if any(l.startswith('late-arm') for l in s['lines'])]
    runs = [l.strip() for l in open(summary) if re.match(r'\S+ (MATCHED|discarded)', l)]
    n = max(len(marmed), len(iarmed), len(runs))
    if not (len(marmed) == len(iarmed) == len(runs)):
        print(f"⚠️ SEGMENT/RUN COUNT MISMATCH: mergelog_armed={len(marmed)} "
              f"ibwatch_armed={len(iarmed)} summary_runs={len(runs)} — "
              f"alignment below is BY ORDER and may be off; check for an "
              f"attempt that armed and then died mid-run.")
    for i in range(n):
        verdict = runs[i] if i < len(runs) else '(no summary line)'
        print(f"== boot {i + 1}: {verdict}")
        if i < len(marmed):
            L = marmed[i]['lines']
            sched = [l for l in L if l.startswith('SCHED')]
            merg = [l for l in L if l.startswith('MERGER')]
            peeks = [l for l in L if l.startswith('PEEK')]
            r4 = {}
            tmax = 0
            for l in merg:
                m = re.search(r't=(\d+)ms r3=\S+ r4=(\S+)', l)
                if m:
                    tmax = max(tmax, int(m.group(1)))
                    r4[m.group(2)] = r4.get(m.group(2), 0) + 1
            top = sorted(r4.items(), key=lambda kv: -kv[1])[:6]
            print(f"   SCHED passes={len(sched)}  MERGER calls={len(merg)} "
                  f"uniq_r4={len(r4)} last_t={tmax}ms")
            print(f"   top r4: " + " ".join(f"{k}x{v}" for k, v in top))
            for p in peeks:
                print(f"   {p}")
        if i < len(iarmed):
            L = iarmed[i]['lines']
            hits = [l for l in L if re.match(r'hit#[1-9]', l)]
            offs = set()
            for l in hits:
                m = re.search(r'pc=0x([0-9a-f]+).* text=0x([0-9a-f]+)', l)
                if m:
                    offs.add(hex(int(m.group(1), 16) - int(m.group(2), 16)))
            arm = next((l for l in L if l.startswith('late-arm')), '')
            print(f"   1B hits={len(hits)} writer_offs={sorted(offs)} {arm}")


if __name__ == '__main__':
    main()
