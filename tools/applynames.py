# Apply confident (OK) names from sdk_matches.txt to a dtk symbols.txt.
# Names occurring more than once (file-static helpers) are made unique with
# an address suffix so dtk/linkers don't collide.
import re, sys, collections
syms, matches = sys.argv[1], sys.argv[2]
names = {}
for l in open(matches):
    p = l.split()
    if p[0] == 'OK': names[int(p[1], 16)] = p[3]
cnt = collections.Counter(names.values())
lines = open(syms).read().splitlines()
existing = {l.split(' = ')[0] for l in lines}
out, n = [], 0
for l in lines:
    m = re.match(r'^(fn_[0-9A-F]+) = \.text:0x([0-9A-F]+);(.*)$', l)
    if m and int(m[2], 16) in names:
        a = int(m[2], 16); nm = names[a]
        if cnt[nm] > 1 or nm in existing: nm = f'{nm}_{a:08X}'
        out.append(f'{nm} = .text:0x{m[2]};{m[3]}'); n += 1
    else: out.append(l)
open(syms, 'w').write('\n'.join(out) + '\n')
print('renamed', n)
