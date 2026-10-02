#!/usr/bin/env python3
"""Decode Microsoft-Windows-DxgKrnl events from a .etl that wpr recorded.

    dxgk-decode.py <events.json> <file.etl> [--all]

events.json maps "id/version" to [[field name, input type], ...]; build it
from the WEVT_TEMPLATE resource of the dxgkrnl.sys that wrote the trace
(make-events-json.py). Without --all, prints only events whose fields carry
a status, reason, failure or message, and those with a failing NTSTATUS.
"""
import json, struct, sys
from etl.etl import build_from_stream

DXGK = "802ec45a-1e99-4b83-9920-87c98277ba9d"
events = json.load(open(sys.argv[1]))
show_all = "--all" in sys.argv

def read(data, off, t):
    # EVT input types (winmeta.xml): 1 UnicodeString, 2 AnsiString, 3 Int8,
    # 4 UInt8, 5 Int16, 6 UInt16, 7 Int32, 8 UInt32, 9 Int64, 10 UInt64,
    # 11 Float, 12 Double, 13 Boolean(4), 14 Binary, 15 GUID, 16 Pointer,
    # 17 FILETIME, 18 SYSTEMTIME, 19 SID, 20 HexInt32, 21 HexInt64
    if t == 1:
        end = off
        while end + 1 < len(data) and data[end:end + 2] != b"\0\0":
            end += 2
        return data[off:end].decode("utf-16-le", "replace"), end + 2
    if t == 2:
        end = data.find(b"\0", off)
        end = len(data) if end < 0 else end
        return data[off:end].decode("latin-1"), end + 1
    size = {3: 1, 4: 1, 5: 2, 6: 2, 7: 4, 8: 4, 9: 8, 10: 8, 11: 4, 12: 8,
            13: 4, 15: 16, 16: 8, 17: 8, 18: 16, 20: 4, 21: 8}.get(t)
    if size is None or off + size > len(data):
        return data[off:].hex(), len(data)
    raw = data[off:off + size]
    if t == 15:
        return str(raw.hex()), off + size
    v = int.from_bytes(raw, "little", signed=t in (3, 5, 7, 9))
    return (hex(v & 0xFFFFFFFFFFFFFFFF) if t in (16, 20, 21) or size >= 4 else v), off + size

def interesting(names, vals):
    low = " ".join(names).lower()
    if any(k in low for k in ("message", "description", "bucket", "fail", "reason")):
        return True
    for n, v in zip(names, vals):
        if "status" in n.lower() and isinstance(v, str) and v.startswith("0xc"):
            return True
    return False

from etl.etl import IEtlFileObserver

def guid_of(h):
    g = h.provider_id.inner if hasattr(h.provider_id, "inner") else h.provider_id
    d4 = bytes(g.data4)
    return "%08x-%04x-%04x-%s-%s" % (g.data1, g.data2, g.data3, d4[:2].hex(), d4[2:].hex())

class Obs(IEtlFileObserver):
    def on_event_record(self, ev):
        h = ev.source.event_header
        if guid_of(h) != DXGK:
            return
        eid, ver = h.event_descriptor.Id, h.event_descriptor.Version
        data = bytes(ev.source.user_data)
        fields = events.get(f"{eid}/{ver}") or events.get(f"{eid}/0") or []
        names, vals, off = [], [], 0
        for name, t in fields:
            v, off = read(data, off, t)
            names.append(name); vals.append(v)
        if show_all or interesting(names, vals):
            print(h.timestamp, eid, ver, dict(zip(names, vals)) if names else data.hex())
    def on_trace_record(self, e): pass
    def on_system_trace(self, e): pass
    def on_perfinfo_trace(self, e): pass
    def on_win_trace(self, e): pass

with open(sys.argv[2], "rb") as f:
    build_from_stream(f.read()).parse(Obs())
