#!/usr/bin/env python3
"""Map the @addresses in 'SyncTest: state differs' log lines to symbols (globals) or memp offsets.
Usage: tools/net/symbolize-synctest.py <log> <binary>   (log from PD_SYNCTEST_FULL=1)"""
import bisect, re, subprocess, sys
log, binary = sys.argv[1], sys.argv[2]
syms = []
for l in subprocess.run(['nm', '-n', binary], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3:
        try: syms.append((int(p[0], 16), p[2]))
        except ValueError: pass
syms.sort(); addrs = [a for a, _ in syms]
# file address of __PDSNAP,__data (runtime range 0 base maps to it)
out = subprocess.run(['otool', '-l', binary], capture_output=True, text=True).stdout.split('\n')
secs = {}
for i, l in enumerate(out):
    if 'segname __PDSNAP' in l:
        secs[out[i-1].split()[-1]] = int(out[i+1].split()[-1], 16)
seen = set()
for line in open(log):
    if 'state differs' not in line: continue
    for part in line.split('[range ')[1:]:
        m = re.match(r'(\d) base (0x[0-9a-f]+)', part)
        rng, base = int(m.group(1)), int(m.group(2), 16)
        for a in re.findall(r'@(0x[0-9a-f]+)', part):
            a = int(a, 16)
            if rng in (0, 1):
                fa = secs['__data' if rng == 0 else '__bss'] + (a - base)
                i = bisect.bisect_right(addrs, fa) - 1
                key = f"{syms[i][1]}+0x{fa - syms[i][0]:x}"
            else:
                key = f"memp range {rng} +0x{a - base:x}"
            if key not in seen:
                seen.add(key); print(key)
