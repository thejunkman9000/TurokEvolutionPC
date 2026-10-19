# Extract main.dol from a GameCube disc image.
# usage: extract_dol.py <disc.iso> <out main.dol>
import struct
import sys

iso, out = sys.argv[1], sys.argv[2]
with open(iso, 'rb') as f:
    hdr = f.read(0x440)
    if hdr[:6] != b'GTKE51':
        sys.exit(f"{iso}: not Turok: Evolution NTSC-U (GTKE51), found {hdr[:6]!r}")
    dol_off = struct.unpack('>I', hdr[0x420:0x424])[0]
    f.seek(dol_off)
    dh = f.read(0x100)
    offs = struct.unpack('>18I', dh[0:0x48])
    sizes = struct.unpack('>18I', dh[0x90:0xD8])
    end = max(o + s for o, s in zip(offs, sizes))
    f.seek(dol_off)
    data = f.read(end)
with open(out, 'wb') as o:
    o.write(data)
print(f'extracted main.dol ({len(data)} bytes)')
