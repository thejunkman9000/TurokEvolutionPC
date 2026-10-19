# Name SDK / MSL / Runtime / TRK functions in Turok by aligning them against a
# reference game built with the same Dolphin SDK (Super Mario Sunshine, GMSE01).
#
# Both binaries link the libraries in the same order and same SDK version, so
# functions present in both have identical sizes and appear in the same order.
# We run a global sequence alignment (Needleman-Wunsch style, free gaps) on the
# size sequences, then keep only matches inside runs of consecutive matches
# (isolated size coincidences are discarded).
#
# usage: sdkmatch.py turok_symbols.txt ref_symbols.txt ref_splits.txt out.txt
import re, sys, bisect

LIBS = ('dolphin/', 'PowerPC_EABI_Support/', 'TRK_MINNOW_DOLPHIN/')
SYM = re.compile(r'^(\S+) = \.text:0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)')

def funcs(path):
    out = []
    for l in open(path, encoding='utf-8', errors='replace'):
        m = SYM.match(l)
        if m: out.append((int(m[2], 16), int(m[3], 16), m[1]))
    return sorted(out)

def lib_ranges(splits):
    cur, out = None, []
    for l in open(splits):
        if l and not l[0].isspace() and l.rstrip().endswith(':'):
            cur = l.rstrip()[:-1]
        m = re.match(r'\s+\.text\s+start:0x([0-9A-Fa-f]+) end:0x([0-9A-Fa-f]+)', l)
        if m and cur and cur.startswith(LIBS):
            out.append((int(m[1], 16), int(m[2], 16), cur))
    return sorted(out)

turok = [f for f in funcs(sys.argv[1]) if f[0] >= 0x80118000]
ranges = lib_ranges(sys.argv[3])
starts = [r[0] for r in ranges]
ref = []
for a, s, n in funcs(sys.argv[2]):
    i = bisect.bisect_right(starts, a) - 1
    if i >= 0 and ranges[i][0] <= a < ranges[i][1]:
        ref.append((a, s, n, ranges[i][2]))

T, R = len(turok), len(ref)
print(f'turok candidates {T}, reference lib functions {R}')
known = {n: a for a, s, n in turok if not n.startswith(('fn_', 'lbl_'))}

# Per-object local alignment: each reference object (translation unit) is
# placed independently, since library link order can differ between games.
# Within an object either side may skip functions (dead-stripping); Turok's
# start/end inside its sequence are free (Smith-Waterman).
def w(t, r):
    if t[1] != r[1]: return None
    if t[2] == r[2]: return 50              # name already known & equal
    if not t[2].startswith(('fn_', 'lbl_')): return None  # known, different
    return 1 + min(t[1], 0x400) / 0x40      # bigger functions = more evidence
GAP = 0.6
objs = {}
for k, r in enumerate(ref): objs.setdefault(r[3], []).append(k)
cands = []
for obj, idx in objs.items():
    K = len(idx)
    H = [[0.0] * (K + 1) for _ in range(T + 1)]
    B = [[0] * (K + 1) for _ in range(T + 1)]
    best = (0, 0, 0)
    for i in range(1, T + 1):
        t = turok[i - 1]; Hi = H[i]; Hp = H[i - 1]; Bi = B[i]
        for j in range(1, K + 1):
            v, b = 0.0, 0
            if Hp[j] - GAP > v: v, b = Hp[j] - GAP, 1        # extra turok fn
            if Hi[j - 1] > v: v, b = Hi[j - 1], 2            # ref fn stripped
            sc = w(t, ref[idx[j - 1]])
            if sc is not None and Hp[j - 1] + sc > v: v, b = Hp[j - 1] + sc, 3
            Hi[j] = v; Bi[j] = b
            if v > best[0]: best = (v, i, j)
    v, i, j = best
    pr = []
    while i > 0 and j > 0 and H[i][j] > 0:
        b = B[i][j]
        if b == 3: pr.append((i - 1, idx[j - 1])); i -= 1; j -= 1
        elif b == 1: i -= 1
        else: j -= 1
    if pr: cands.append((v, obj, pr[::-1]))
# Greedy: highest scoring objects claim their turok functions first.
used = set(); pairs = []; keep = set()
for v, obj, pr in sorted(cands, key=lambda c: -c[0]):
    if any(ti in used for ti, _ in pr): continue
    used.update(ti for ti, _ in pr); pairs += pr
    big = sum(1 for ti, _ in pr if turok[ti][1] >= 0x40)
    if len(pr) >= 3 or big >= 2 or any(turok[ti][2] == ref[ri][2] for ti, ri in pr):
        keep.update(pr)
pairs.sort()

with open(sys.argv[4], 'w') as o:
    for ti, ri in pairs:
        t, r = turok[ti], ref[ri]
        tag = 'OK ' if (ti, ri) in keep else 'WEAK'
        o.write(f'{tag} {t[0]:08x} {t[1]:#7x} {r[2]:40s} {r[3]}\n')
print(f'aligned {len(pairs)}, confident {len(keep)}')
bad = [(n, a) for (ti, ri) in keep for n, a in [(turok[ti][2], turok[ti][0])]
       if not n.startswith(('fn_', 'lbl_')) and n != ref[ri][2]]
print('conflicts with dtk names:', bad)
print('dtk-known names confirmed:', sum(1 for ti, ri in keep if turok[ti][2] == ref[ri][2]), 'of', len(known))
