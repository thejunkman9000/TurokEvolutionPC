# Scan the game's data archive (supertree0.tre inside the disc image) for
# embedded level paths (*.atr under Data/Levels).
# usage: scan_archive_paths.py <disc.iso>
import re
import sys

ARCHIVE_OFFSET, ARCHIVE_SIZE = 613648544, 846329672  # from the disc FST
pat = re.compile(rb'[\w\-./\\$]*[Ll]evels[/\\][\w\-./\\]+?\.atr', re.I)
found = {}
with open(sys.argv[1], 'rb') as f:
    f.seek(ARCHIVE_OFFSET)
    left, carry = ARCHIVE_SIZE, b''
    while left > 0:
        chunk = f.read(min(left, 64 << 20))
        left -= len(chunk)
        buf = carry + chunk
        for m in pat.finditer(buf):
            p = m.group().decode('latin-1').replace('\\', '/').lstrip('$/').lower()
            found[p] = found.get(p, 0) + 1
        carry = buf[-512:]
for p in sorted(found):
    print(found[p], p)
print(len(found), 'distinct level paths', file=sys.stderr)
