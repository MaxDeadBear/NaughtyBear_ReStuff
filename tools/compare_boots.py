#!/usr/bin/env python3
"""Diff what the XAM layer did on SIGNIN boots vs GAMEPLAY boots.

Per-boot logs (M3.260 RESTUFF_LOGPID=1) are paired to frame dirs by mtime, so
attribution is exact -- the earlier shared-log ordering was not, and a claim was
built on it that should not have been.
"""
import os, sys, glob, re
sys.path.insert(0, "tools")
from classify_boot2 import sig, dist, REFS

D = "/tmp/restuff_drive"
prefix = sys.argv[1] if len(sys.argv) > 1 else "env_sig1"
refs = {k: sig(p) for k, p in REFS.items() if os.path.exists(p)}
logs = []
for f in glob.glob(f"{D}/mergelog_*.txt"):
    try: logs.append((os.stat(f).st_mtime, f))
    except OSError: pass

def pair(frame):
    ft = os.stat(frame).st_mtime
    best, bd = None, 1e18
    for lt, f in logs:
        d = abs(lt - ft)
        if d < bd: bd, best = d, f
    return best, bd

groups = {}
for name in sorted(os.listdir(D)):
    if not name.startswith(prefix): continue
    frame = os.path.join(D, name, "W01.png")
    if not os.path.exists(frame): continue
    try: s = sig(frame)
    except Exception: continue
    kind, _ = min(((k, dist(s, v)) for k, v in refs.items()), key=lambda kv: kv[1])
    lg, dt = pair(frame)
    groups.setdefault(kind, []).append((name, lg, dt))

for kind, items in sorted(groups.items()):
    print("\n########## %s (%d boots) ##########" % (kind, len(items)))
    for name, lg, dt in items[:6]:
        print("-- %s   log=%s dt=%.0fs" % (name, os.path.basename(lg or "?"), dt))
        if not lg or not os.path.exists(lg): continue
        txt = open(lg, errors="ignore").read()
        for pat, label in (("SIGNCACHE#", "cache"), ("XAM#", "xam"),
                           ("SIGNINFIX", "fix"), ("MSGBOX", "msgbox"),
                           ("ONLINESM", "onlinesm")):
            hits = [l for l in txt.splitlines() if l.startswith(pat)]
            if hits:
                print("     %-8s x%-3d %s" % (label, len(hits), hits[0][:110]))
                # any query for a user other than the 4-slot sweep?
                if pat == "XAM#":
                    odd = [l for l in hits if "lr=82B2F750" not in l]
                    for o in odd[:4]:
                        print("        ODD-LR %s" % o[:110])
