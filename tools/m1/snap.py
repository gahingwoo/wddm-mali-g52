# SPDX-License-Identifier: GPL-2.0
# Snapshot the named PMU/CRU registers that matter for PD_GPU and the GPU
# clock, as JSON. Named registers only: a blind sweep of the PMU/CRU blocks
# hit a hole and took the SoC down with an SError (2026-10-01).
import json, mmap, os, struct, sys

PMU, CRU = 0x27380000, 0x27200000
regs = {
    "PMU REQ0": PMU + 0x110, "PMU REQ1": PMU + 0x114,
    "PMU ACK0": PMU + 0x120, "PMU ACK1": PMU + 0x124,
    "PMU IDLE0": PMU + 0x128, "PMU IDLE1": PMU + 0x12c,
    "PMU CLK_UNGATE": PMU + 0x140,
    "PMU PWR_CON0": PMU + 0x210, "PMU PWR_CON1": PMU + 0x214,
    "PMU STATUS0": PMU + 0x230, "PMU STATUS1": PMU + 0x234,
    "PMU CHAIN0": PMU + 0x248, "PMU CHAIN1": PMU + 0x24c,
    "PMU MEMST0": PMU + 0x250, "PMU MEMST1": PMU + 0x254,
    "PMU MEMPWR0": PMU + 0x300, "PMU MEMPWR1": PMU + 0x304,
    "PMU REPAIR0": PMU + 0x570, "PMU REPAIR1": PMU + 0x574,
    "CRU AUPLL0": CRU + 0x180, "CRU AUPLL1": CRU + 0x184,       # RK3576_PLL_CON(96..97)
    "CRU MODE_CON0": CRU + 0x280,
    "CRU CLKSEL165": CRU + 0x300 + 165 * 4, "CRU CLKSEL166": CRU + 0x300 + 166 * 4,
    "CRU GATE69": CRU + 0x800 + 69 * 4, "CRU SRST69": CRU + 0xa00 + 69 * 4,
}
f = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
maps, out = {}, {}
for name, a in regs.items():
    b = a & ~0xfff
    if b not in maps:
        maps[b] = mmap.mmap(f, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=b)
    out[name] = struct.unpack_from("<I", maps[b], a - b)[0]
json.dump(out, open(sys.argv[1], "w"), indent=0)
