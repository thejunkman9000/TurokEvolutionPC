import struct, sys, re, collections
from capstone import *
dol = open(sys.argv[1], 'rb').read()
out = sys.argv[2]
off = struct.unpack('>18I', dol[0:0x48]); addr = struct.unpack('>18I', dol[0x48:0x90]); size = struct.unpack('>18I', dol[0x90:0xd8])
bss, bsssz, entry = struct.unpack('>III', dol[0xd8:0xe4])
secs = []
for i in range(18):
    if size[i]:
        secs.append(('text' if i < 7 else 'data', i, off[i], addr[i], size[i]))
        print(f"{'T' if i<7 else 'D'}{i if i<7 else i-7} file={off[i]:#x} addr={addr[i]:#010x} size={size[i]:#x}")
print(f'bss={bss:#x} size={bsssz:#x} entry={entry:#x}')
md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN | CS_MODE_PS)
md.detail = False
funcs = set([entry]); calls = collections.Counter(); ninsn = 0; bad = 0
lines = []
for kind, i, o, a, s in secs:
    if kind != 'text': continue
    code = dol[o:o+s]
    pos = 0
    while pos < s:
        got = False
        for ins in md.disasm(code[pos:], a + pos):
            got = True
            ninsn += 1
            lines.append(f'{ins.address:08x}: {code[ins.address-a:ins.address-a+4].hex()}  {ins.mnemonic:8s} {ins.op_str}')
            if ins.mnemonic == 'bl':
                try:
                    t = int(ins.op_str, 0); funcs.add(t); calls[t] += 1
                except ValueError: pass
            pos = ins.address - a + 4
        if pos < s and not got or (pos < s and got and (a+pos) != a+pos):
            pass
        if pos < s:
            w = code[pos:pos+4]
            lines.append(f'{a+pos:08x}: {w.hex()}  .long    0x{w.hex()}')
            bad += 1; pos += 4
open(out + '/main.asm', 'w').write('\n'.join(lines))
# prologue-based function starts
fs = set(funcs)
for kind, i, o, a, s in secs:
    if kind != 'text': continue
    for p in range(0, s - 8, 4):
        w = struct.unpack('>I', dol[o+p:o+p+4])[0]
        if w == 0x7c0802a6 or (w >> 16) == 0x9421:  # mflr r0 / stwu r1
            pass
        if (w >> 16) == 0x9421 and a + p not in fs: fs.add(a + p)
text_ranges = [(a, a+s) for k, i, o, a, s in secs if k == 'text']
fs = sorted(x for x in fs if any(lo <= x < hi for lo, hi in text_ranges))
open(out + '/functions.txt', 'w').write('\n'.join(f'{x:08x} {calls.get(x,0)}' for x in fs))
print('instructions', ninsn, 'undecoded', bad, 'functions', len(fs))
# strings in data sections
strs = []
for kind, i, o, a, s in secs:
    blob = dol[o:o+s]
    for m in re.finditer(rb'[\x20-\x7e]{6,}', blob):
        strs.append((a + m.start(), m.group().decode()))
open(out + '/strings.txt', 'w').write('\n'.join(f'{x:08x} {t}' for x, t in strs))
print('strings', len(strs))
