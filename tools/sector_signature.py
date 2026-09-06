#!/usr/bin/env python3
"""Per-boot GAMEPLAY sector-table signature over time (task #24).

⚠️ THE SIGNATURE IS NOT THE FAMILY — mk1 refuted that (a MERGED boot scored
1/1/1/1 while a WEDGE boot scored 6/6/5/5). mrg37's apparent correlation rested
on a single merged boot. The table IS frozen from first visibility, but it is a
BYSTANDER: never classify a boot from this output. Kept because the freeze and
the two-context split are real structural facts worth measuring.

Two contexts run through sub_829CA0C8 and MUST NOT be mixed: the front-end one
(r3=A660C7E0, table A6617C80, identical in every boot, optional blocks never
populated) and the per-level gameplay one. Only the latter is reported here.

Signature = how many of the 64 sectors have each optional block populated:
    +4..11 / +24..27 / +32..39 / +52..55
Observed across boots with NO relation to outcome: wedge 2/0/1/2, 12/9/6/6,
7/9/7/4, 6/6/5/5, 0/0/0/0; merged 14/10/11/11, 7/4/5/6, 1/1/1/1.

Usage: sector_signature.py <mergelog> [...]
"""
import re
import sys

FRONTEND_CTX = 'A660C7E0'
BLOCKS = [('+4..11', 4, 12), ('+24..27', 24, 28), ('+32..39', 32, 40), ('+52..55', 52, 56)]


def signature(d):
    """d = hex dump of the 64x56-byte table; returns (counts, live, populated)."""
    counts = [0] * len(BLOCKS)
    live, pop = [], []
    for s in range(64):
        e = d[112 * s:112 * s + 112]
        if len(e) < 112:
            continue
        if any(e[2 * o:2 * o + 2] != '00' for o in range(12, 24)):
            live.append(s)
        hit = False
        for i, (_, a, b) in enumerate(BLOCKS):
            if any(e[2 * o:2 * o + 2] != '00' for o in range(a, b)):
                counts[i] += 1
                hit = True
        if hit:
            pop.append(s)
    return counts, live, pop


def boots(path):
    out, cur, r3 = [], None, None
    for ln in open(path, errors='replace'):
        if ln.startswith('PEEK t='):
            m = re.search(r'PEEK t=(\d+)ms', ln)
            if m and int(m.group(1)) < 20000:
                cur = {'stab': [], 'force': []}
                out.append(cur)
        elif cur is None:
            continue
        elif ln.startswith('SCHED '):
            m = re.match(r'SCHED t=\d+ms bits=[0-9A-F]+ r3=([0-9A-F]+)', ln)
            if m:
                r3 = m.group(1)
        elif ln.startswith('FORCE#'):
            m = re.match(r'FORCE#(\d+) t=(\d+)ms', ln)
            if m:
                cur['force'].append(int(m.group(2)))
        elif ln.startswith('STAB '):
            m = re.match(r'STAB t=(\d+)ms tab=[0-9A-F]+ d=([0-9A-F]+)', ln)
            if m and r3 and r3 != FRONTEND_CTX:
                cur['stab'].append((int(m.group(1)), m.group(2)))
    return out


for path in sys.argv[1:]:
    print(f"\n########## {path}")
    for i, b in enumerate(boots(path), 1):
        if not b['stab']:
            print(f"  boot{i}: no gameplay-context samples (never reached level?)")
            continue
        first_t, first_d = b['stab'][0]
        last_t, last_d = b['stab'][-1]
        c0, live, pop0 = signature(first_d)
        c1, _, pop1 = signature(last_d)
        fmt = lambda c: "/".join(str(v) for v in c)
        print(f"  boot{i}: samples={len(b['stab'])} t={first_t}..{last_t}")
        print(f"      signature first={fmt(c0)}  last={fmt(c1)}"
              f"   {'CHANGED' if c0 != c1 else 'frozen'}")
        print(f"      live sectors={live}")
        print(f"      populated first={pop0}")
        if pop1 != pop0:
            print(f"      populated last ={pop1}   gained={sorted(set(pop1)-set(pop0))}")
        if b['force']:
            print(f"      FORCE fired {len(b['force'])}x  t={b['force'][0]}..{b['force'][-1]}ms")
            after = [(t, signature(d)[0]) for t, d in b['stab'] if t > b['force'][0]]
            if after:
                print(f"      signature after first FORCE: {fmt(after[0][1])} -> {fmt(after[-1][1])}")
        else:
            print("      FORCE never fired")
