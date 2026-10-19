# Name game functions from the strings they reference.
#
# Scans every dtk function for address constants built with lis/addi, lis/ori
# or r2/r13 small-data offsets, resolves them to strings, then:
#   * "Class::Method ..." strings -> proposes the name Class::Method
#   * "C:/projects/.../File.cpp" strings -> tags the function with a source file
#
# usage: strnames.py main.dol symbols.txt out_names.txt out_sources.txt
import re, struct, sys, collections

dol = open(sys.argv[1], 'rb').read()
off = struct.unpack('>18I', dol[0:0x48]); addr = struct.unpack('>18I', dol[0x48:0x90]); size = struct.unpack('>18I', dol[0x90:0xd8])
SECS = [(addr[i], size[i], off[i]) for i in range(18) if size[i]]
R2, R13 = 0x803289c0, 0x803189c0

def rd(a, n):
    for b, s, o in SECS:
        if b <= a and a + n <= b + s: return dol[o + a - b: o + a - b + n]
    return None

def cstr(a):
    out = bytearray()
    while len(out) < 200:
        c = rd(a + len(out), 1)
        if not c or c == b'\0': break
        out += c
    try: s = out.decode('ascii')
    except UnicodeDecodeError: return None
    return s if len(s) >= 4 and all(32 <= ord(ch) < 127 for ch in s) else None

funcs = []
for l in open(sys.argv[2]):
    m = re.match(r'^(\S+) = \.text:0x([0-9A-F]+); // type:function size:0x([0-9A-F]+)', l)
    if m: funcs.append((int(m[2], 16), int(m[3], 16), m[1]))

METHOD = re.compile(r'^\s*(?:\w+:\s*)?((?:[A-Za-z_]\w*::)+~?[A-Za-z_]\w*)\b')
SRC = re.compile(r'([A-Za-z]:[/\\][^"]*?\.(?:cpp|c|h|hpp))', re.I)

names, sources = {}, {}
for fa, fs, fname in funcs:
    hi = {}; refs = []
    for p in range(fa, fa + fs, 4):
        w = rd(p, 4)
        if not w: break
        w = struct.unpack('>I', w)[0]; op = w >> 26
        rD = (w >> 21) & 31; rA = (w >> 16) & 31; imm = w & 0xffff
        simm = imm - 0x10000 if imm & 0x8000 else imm
        if op == 15 and rA == 0: hi[rD] = imm << 16; continue      # lis
        if op == 14:                                                  # addi
            if rA in hi: refs.append((hi[rA] + simm) & 0xffffffff)
            elif rA == 2: refs.append(R2 + simm)
            elif rA == 13: refs.append(R13 + simm)
        elif op == 24 and rD in hi:                                   # ori rA,rS
            refs.append(hi[rD] | imm)
        if op in (14, 15, 24, 32) and rD in hi and op != 24: hi.pop(rD, None)
    meth, src = collections.Counter(), set()
    for r in refs:
        s = cstr(r)
        if not s: continue
        m = METHOD.match(s)
        if m: meth[m[1]] += 1
        for m in SRC.finditer(s): src.add(m[1].replace('\\', '/'))
    if meth: names[fa] = (fname, meth)
    if src: sources[fa] = (fname, sorted(src))

with open(sys.argv[3], 'w') as o:
    single = 0
    for fa in sorted(names):
        fname, meth = names[fa]
        conf = 'OK ' if len(meth) == 1 else 'MULTI'
        single += len(meth) == 1
        o.write(f'{conf} {fa:08x} {fname:24s} ' + ' | '.join(f'{k}x{v}' for k, v in meth.most_common()) + '\n')
with open(sys.argv[4], 'w') as o:
    for fa in sorted(sources):
        o.write(f'{fa:08x} {sources[fa][0]:24s} {" ".join(sources[fa][1])}\n')
print(f'functions {len(funcs)}; with Class::Method strings {len(names)} (unambiguous {single}); with source paths {len(sources)}')
