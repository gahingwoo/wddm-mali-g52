#!/usr/bin/env python3
"""Names of Microsoft-Windows-DxgKrnl events, from a dxgkrnl.sys.

    dxgk-event-names.py dxgkrnl.sys [event id ...]

Prints "id version task <n> <task name> opcode <n> <opcode name> level <n>"
for each event (or only the ids given). dxgk-decode.py prints events by
id; this gives the names, which pyfwevt (make-events-json.py) does not
expose: they are in the EVNT, TASK and OPCO tables of the WEVT_TEMPLATE
resource, read here directly. Needs pefile.
"""
import struct, sys
import pefile

pe = pefile.PE(sys.argv[1])
blob = None
for e in pe.DIRECTORY_ENTRY_RESOURCE.entries:
    if str(e.name) == "WEVT_TEMPLATE":
        d = e.directory.entries[0].directory.entries[0].data.struct
        blob = pe.get_data(d.OffsetToData, d.Size)
if blob is None:
    sys.exit("no WEVT_TEMPLATE resource")


def string(off):
    if off <= 0 or off + 4 > len(blob):
        return None
    n = struct.unpack_from("<I", blob, off)[0]
    if n > 400:
        return None
    return blob[off + 4:off + n].decode("utf-16-le", "replace").rstrip("\x00")


tasks = {}
i = blob.find(b"TASK")
_, count = struct.unpack_from("<II", blob, i + 4)
for k in range(count):
    tid = struct.unpack_from("<I", blob, i + 12 + 28 * k)[0]
    tasks[tid] = string(struct.unpack_from("<I", blob, i + 12 + 28 * k + 24)[0])

opcodes = {}
i = blob.find(b"OPCO")
_, count = struct.unpack_from("<II", blob, i + 4)
for k in range(count):
    oid, _msg, doff = struct.unpack_from("<III", blob, i + 12 + 12 * k)
    opcodes[oid] = string(doff)

want = {int(a) for a in sys.argv[2:]}
i = blob.find(b"EVNT")
_, count, _ = struct.unpack_from("<III", blob, i + 4)
off = i + 16
for k in range(count):
    eid, ver, _ch, level, opcode, task = struct.unpack_from("<HBBBBH", blob, off)
    if not want or eid in want:
        name = opcodes.get(opcode) or opcodes.get((task << 16) | opcode) or opcodes.get(opcode << 16)
        print(eid, ver, "task", task, tasks.get(task), "opcode", opcode, name, "level", level)
    off += 48
