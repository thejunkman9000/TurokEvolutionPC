# Find CodeWarrior C++ vtables in the DOL data sections and recover class names
# from RTTI where present.
#
# CW vtable layout: [ RTTI* | this-adjust (0) | fn* fn* fn* ... ]
# CW RTTI object:   [ char* class_name | base_list* ]
#
# usage: vtables.py main.dol symbols.txt out.txt
import re, struct, sys

dol = open(sys.argv[1], 'rb').read()
off = struct.unpack('>18I', dol[0:0x48]); addr = struct.unpack('>18I', dol[0x48:0x90]); size = struct.unpack('>18I', dol[0x90:0xd8])
TEXT = [(addr[i], addr[i] + size[i]) for i in range(7) if size[i]]
DATA = [(addr[i], size[i], off[i]) for i in range(7, 18) if size[i]]
funcs = {}
for l in open(sys.argv[2]):
    m = re.match(r'^(\S+) = \.text:0x([0-9A-F]+); // type:function', l)
    if m: funcs[int(m[2], 16)] = m[1]

def rd32(a):
    for b, s, o in DATA:
        if b <= a and a + 4 <= b + s: return struct.unpack('>I', dol[o + a - b: o + a - b + 4])[0]
    return None

def cstr(a):
    for b, s, o in DATA:
        if b <= a < b + s:
            e = dol.find(b'\0', o + a - b, o + s)
            try: t = dol[o + a - b: e].decode('ascii')
            except UnicodeDecodeError: return None
            return t if 1 <= len(t) < 200 and t.isprintable() else None
    return None

def is_fn(v): return v in funcs
vtables = []
for b, s, o in DATA:
    a = b
    while a + 12 <= b + s:
        rtti, adj = rd32(a), rd32(a + 4)
        if adj == 0 and (rtti == 0 or (rtti and not is_fn(rtti))) and is_fn(rd32(a + 8) or 0):
            n = 0
            while (v := rd32(a + 8 + 4 * n)) is not None and (is_fn(v) or v == 0) and n < 400:
                if v == 0 and not is_fn(rd32(a + 12 + 4 * n) or 0): break
                n += 1
            if n >= 2:
                cls = None
                if rtti:
                    np = rd32(rtti)
                    cls = cstr(np) if np else None
                vtables.append((a, rtti, cls, [rd32(a + 8 + 4 * k) for k in range(n)]))
                a += 8 + 4 * n; continue
        a += 4
named = [v for v in vtables if v[2]]
with open(sys.argv[3], 'w') as o:
    for a, rtti, cls, fs in vtables:
        o.write(f'{a:08x} rtti={rtti or 0:08x} class={cls or "?"} n={len(fs)} ' + ' '.join(f'{f:08x}' for f in fs) + '\n')
print(f'vtables {len(vtables)}, with RTTI class name {len(named)}, '
      f'virtual slots {sum(len(v[3]) for v in vtables)}, distinct targets {len({f for v in vtables for f in v[3]})}')
for v in named[:25]: print(f'  {v[0]:08x} {v[2]} ({len(v[3])} slots)')
