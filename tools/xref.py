# Find functions that build a given address (lis+addi / lis+ori / r2,r13 SDA).
# usage: xref.py main.dol symbols.txt <hex address> [<hex address> ...]
import re, struct, sys

dol = open(sys.argv[1], 'rb').read()
off = struct.unpack('>18I', dol[0:0x48]); addr = struct.unpack('>18I', dol[0x48:0x90]); size = struct.unpack('>18I', dol[0x90:0xd8])
SECS = [(addr[i], size[i], off[i]) for i in range(18) if size[i]]
R2, R13 = 0x803289c0, 0x803189c0
targets = {int(a, 16) for a in sys.argv[3:]}

def rd(a):
    for b, s, o in SECS:
        if b <= a and a + 4 <= b + s: return struct.unpack('>I', dol[o + a - b:o + a - b + 4])[0]
    return None

funcs = []
for l in open(sys.argv[2]):
    m = re.match(r'^(\S+) = \.text:0x([0-9A-F]+); // type:function size:0x([0-9A-F]+)', l)
    if m: funcs.append((int(m[2], 16), int(m[3], 16), m[1]))

for fa, fs, name in funcs:
    hi = {}
    for p in range(fa, fa + fs, 4):
        w = rd(p)
        if w is None: break
        op = w >> 26; rd_ = (w >> 21) & 31; ra = (w >> 16) & 31; imm = w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        v = None
        if op == 15 and ra == 0: hi[rd_] = imm << 16; continue
        if op == 14:
            if ra in hi: v = (hi[ra] + simm) & 0xFFFFFFFF
            elif ra == 2: v = R2 + simm
            elif ra == 13: v = R13 + simm
        elif op == 24 and rd_ in hi: v = hi[rd_] | imm
        elif op in (32, 34, 40, 36) and ra in (2, 13):  # lwz/lbz/lhz/stw r, d(r2/r13)
            v = (R2 if ra == 2 else R13) + simm
        if v in targets: print(f'{v:08X} referenced by {name} @ {p:08X}')
