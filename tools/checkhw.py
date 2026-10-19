# Cross-check SDK names from sdkmatch.py against hardware-register usage.
hw = {}
for l in open('analysis/hw_functions.txt'):
    a, c = l.rstrip('\n').split(None, 1); hw[int(a, 16)] = c
m = {}
for l in open('analysis/sdk_matches.txt'):
    p = l.split()
    m[int(p[1], 16)] = (p[0], p[3], p[4])
expect = {'dolphin/dvd': 'DI', 'dolphin/si': 'SI', 'dolphin/vi': 'VI', 'dolphin/ai': 'AI', 'dolphin/ar': 'DSP',
          'dolphin/dsp': 'DSP', 'dolphin/exi': 'EXI', 'dolphin/gx': 'GX', 'dolphin/pad': 'SI', 'dolphin/os': ''}
agree = dis = 0; unk = []
for a, c in hw.items():
    if a not in m: unk.append((a, c)); continue
    lib = '/'.join(m[a][2].split('/')[:2])
    e = expect.get(lib)
    if e is None: print('  lib w/o expectation', hex(a), c, m[a][1], lib); continue
    if e in c or (e == 'GX' and any(x in c for x in ('CP', 'PE', 'GX'))): agree += 1
    else: dis += 1; print('  differs', hex(a), c, m[a][1], lib)
print('hw-consistent', agree, 'inconsistent', dis)
sdk_unk = [x for x in unk if 0x8011b000 <= x[0] < 0x80147000]
print('hw funcs in SDK region without a name:', len(sdk_unk), [(hex(a), c) for a, c in sdk_unk])
print('hw funcs outside SDK region (game code):', [(hex(a), c) for a, c in unk if not 0x8011b000 <= a < 0x80147000])
