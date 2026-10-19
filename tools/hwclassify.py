# Classify functions by GameCube hardware register access (lis rX,0xCC00/0xCC01 + d(rX))
import struct, sys, collections
dol = open(sys.argv[1], 'rb').read()
funcs = [int(l.split()[0], 16) for l in open(sys.argv[2])]
off = struct.unpack('>18I', dol[0:0x48]); addr = struct.unpack('>18I', dol[0x48:0x90]); size = struct.unpack('>18I', dol[0x90:0xd8])
def word(a):
    for i in range(7):
        if size[i] and addr[i] <= a < addr[i] + size[i]:
            return struct.unpack('>I', dol[off[i] + a - addr[i]: off[i] + a - addr[i] + 4])[0]
    return None
HW = {0x0000:'CP',0x1000:'PE',0x2000:'VI',0x3000:'PI',0x4000:'MI',0x5000:'DSP/AR',
      0x6000:'DI(DVD)',0x6400:'SI(PAD)',0x6800:'EXI(CARD/SRAM)',0x6c00:'AI',0x8000:'GX FIFO'}
def sub(o):
    if not 0 <= o < 0x10000: return 'other'
    best = None
    for k in sorted(HW):
        if o >= k: best = k
    return HW[best]
res = collections.defaultdict(set)
ends = funcs[1:] + [funcs[-1] + 0x400]
for f, e in zip(funcs, ends):
    regs = {}
    a = f
    while a < e:
        w = word(a)
        if w is None: break
        op = w >> 26
        if op == 15 and ((w >> 16) & 31) == 0:  # lis rD, imm
            rd = (w >> 21) & 31; imm = w & 0xffff
            if imm in (0xcc00, 0xcc01): regs[rd] = imm << 16
            elif rd in regs: del regs[rd]
        elif op in (32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,50,52,54,24,14):
            ra = (w >> 16) & 31; d = w & 0xffff
            if d >= 0x8000: d -= 0x10000
            if ra in regs and op not in (24,14):
                hw = (regs[ra] + d) & 0xffffffff
                res[f].add(sub(hw - 0xcc000000))
            elif ra in regs and op in (24,14):  # ori/addi make full addr in another reg
                rd = (w >> 21) & 31
                regs[rd] = (regs[ra] + (d if op == 14 else (w & 0xffff))) & 0xffffffff
                res[f].add(sub(regs[rd] - 0xcc000000))
        a += 4
by = collections.defaultdict(list)
for f, s in res.items():
    for x in s: by[x].append(f)
for k in sorted(by):
    v = sorted(by[k])
    print(f'{k:16s} {len(v):4d} funcs  range {v[0]:08x}-{v[-1]:08x}')
with open(sys.argv[3], 'w') as o:
    for f in sorted(res):
        o.write(f'{f:08x} {",".join(sorted(res[f]))}\n')
