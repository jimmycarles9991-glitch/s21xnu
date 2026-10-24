import re, struct, sys
ex = []
for l in open(sys.argv[1]):
    m = re.match(r'\s*\d+:\s+(\d+)\.\.\s*(\d+):\s+(\d+)\.\.\s*(\d+):\s+(\d+):(.*)', l)
    if m:
        assert 'unwritten' not in m[6]
        ex.append((int(m[1]), int(m[3]), int(m[5])))
tot = 0
for lo, ph, n in ex:
    assert lo == tot and ph < 2**32
    tot += n
open(sys.argv[2], 'wb').write(b''.join(struct.pack('<III', *e) for e in ex))
print('extents', len(ex), 'blocks', tot)
