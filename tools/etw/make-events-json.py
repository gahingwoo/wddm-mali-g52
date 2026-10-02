#!/usr/bin/env python3
"""Field names of every Microsoft-Windows-DxgKrnl event, from the
WEVT_TEMPLATE resource of a dxgkrnl.sys (take it from the WinPE or Windows
that recorded the trace: the manifest changes between builds).

    make-events-json.py dxgkrnl.sys > events.json

Needs pefile and libfwevt-python.
"""
import json, sys
import pefile, pyfwevt

pe = pefile.PE(sys.argv[1])
blob = None
for e in pe.DIRECTORY_ENTRY_RESOURCE.entries:
    if str(e.name) == "WEVT_TEMPLATE":
        d = e.directory.entries[0].directory.entries[0].data.struct
        blob = pe.get_data(d.OffsetToData, d.Size)
m = pyfwevt.manifest()
m.copy_from_byte_stream(blob)
p = m.get_provider(0)
out = {}
for i in range(p.number_of_events):
    ev = p.get_event(i)
    fields = []
    if ev.template_offset:
        t = p.get_template_by_offset(ev.template_offset)
        fields = [(t.get_item(j).name, t.get_item(j).input_data_type) for j in range(t.number_of_items)]
    out[f"{ev.identifier}/{ev.version}"] = fields
json.dump(out, sys.stdout)
