# SPDX-License-Identifier: GPL-2.0
# Read every Mali address space's registers and walk the page table of the one
# in use for a GPU VA. Run as root while the GPU is powered.
# Mali LPAE (Panfrost's format on G52): 4 levels, 4 KiB granule; table entries
# have type 3, leaf entries type 1 at every level.
import mmap, os, struct, sys
f = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
# STRICT_DEVMEM forbids /dev/mem on RAM, so page tables are read through
# /proc/kcore: an ELF core of the kernel's mappings whose PT_LOAD headers
# carry the physical address of each segment.
kc = open("/proc/kcore", "rb")
_eh = kc.read(64)
_phoff, = struct.unpack_from("<Q", _eh, 32)
_phentsize, _phnum = struct.unpack_from("<HH", _eh, 54)
_segs = []
for i in range(_phnum):
    kc.seek(_phoff + i * _phentsize)
    t, fl, off, va_, pa, fsz, msz, al = struct.unpack("<IIQQQQQQ", kc.read(56))
    if t == 1 and pa != 0xffffffffffffffff:
        _segs.append((pa, fsz, off))
def ram(a):
    for pa, sz, off in _segs:
        if pa <= a < pa + sz:
            kc.seek(off + a - pa)
            return struct.unpack("<Q", kc.read(8))[0]
    raise ValueError("phys %x not in kcore" % a)
def rd(a, n=4):
    b = a & ~0xfff
    m = mmap.mmap(f, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=b)
    v = struct.unpack_from("<Q" if n == 8 else "<I", m, a - b)[0]
    m.close()
    return v
G = 0x27800000
va = int(sys.argv[1], 0)
for a in range(8):
    base = G + 0x2400 + a * 0x40
    tt = rd(base) | rd(base + 4) << 32
    ma = rd(base + 8) | rd(base + 0xc) << 32
    tc = rd(base + 0x30) | rd(base + 0x34) << 32
    st = rd(base + 0x28)
    print("AS%d TRANSTAB=%016x MEMATTR=%016x TRANSCFG=%016x STATUS=%08x" % (a, tt, ma, tc, st))
    if tt == 0:
        continue
    table = tt & ~0xfff & ((1 << 48) - 1)
    for lvl, shift in enumerate((39, 30, 21, 12)):
        idx = (va >> shift) & 0x1ff
        e = ram(table + idx * 8)
        print("   L%d[%3d] @%010x = %016x type=%d" % (lvl, idx, table + idx * 8, e, e & 3))
        if e & 3 != 3 or lvl == 3:
            break
        table = e & 0x0000fffffffff000
