#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Disassemble a Windows graphics kernel binary with its public symbols.

    dxgk-annotate.py <out-dir> --winbindex dxgkrnl.sys 10.0.22621.2428
    dxgk-annotate.py <out-dir> --local path/to/dxgkrnl.sys

--winbindex takes the amd64 build of that version (winbindex.m417z.com only
indexes amd64; the code is the same source as the board's ARM64 one).
--local takes a binary copied from the board, ARM64 or not. Either way the
PDB comes from Microsoft's public symbol server, and <out-dir> gets:

    <name>            the binary
    <name>.pdb        its public symbols
    syms.txt          "<va> <decorated name>", sorted
    dis.txt           the whole image, every function under its name,
                      calls and branches with the target's name
    strings.txt       every UTF-16 string of 12+ characters in it, with its
                      address: dxgkrnl's own explanations of why it refuses
                      a driver are there ("Driver is compiled against ...")

Then: grep strings.txt for a message, grep dis.txt for "lea .*<address>",
and read the function around it. This is how the 2026-11-23 refusal was
found (docs/WDDM2.md 16.6).

Needs llvm-objdump and llvm-pdbutil (LLVM), curl, and pefile.
"""
import bisect, gzip, json, os, re, subprocess, sys, uuid

import pefile

SYMSRV = "https://msdl.microsoft.com/download/symbols"
WINBINDEX = "https://winbindex.m417z.com/data/by_filename_compressed/%s.json.gz"
UA = "Microsoft-Symbol-Server/10.0.0.0"


def fetch(url, path):
    subprocess.run(["curl", "-sSfL", "-m", "900", "-A", UA, "-o", path, url], check=True)


def from_winbindex(name, version, out):
    idx = os.path.join(out, name + ".winbindex.json.gz")
    fetch(WINBINDEX % name, idx)
    for v in json.load(gzip.open(idx)).values():
        fi = v.get("fileInfo", {})
        if fi.get("version", "").split(" ")[0] == version and fi.get("machineType") == 0x8664:
            key = "%08X%x" % (fi["timestamp"], fi["virtualSize"])
            path = os.path.join(out, name)
            fetch("%s/%s/%s/%s" % (SYMSRV, name, key, name), path)
            return path
    sys.exit("%s %s is not in winbindex" % (name, version))


def fetch_pdb(pe, out):
    for d in pe.DIRECTORY_ENTRY_DEBUG:
        if d.struct.Type != 2:      # IMAGE_DEBUG_TYPE_CODEVIEW
            continue
        raw = pe.get_data(d.struct.AddressOfRawData, d.struct.SizeOfData)
        if raw[:4] != b"RSDS":
            continue
        guid = uuid.UUID(bytes_le=raw[4:20]).hex.upper()
        age = int.from_bytes(raw[20:24], "little")
        pdb = raw[24:].split(b"\0")[0].decode().split("\\")[-1]
        path = os.path.join(out, pdb)
        fetch("%s/%s/%s%X/%s" % (SYMSRV, pdb, guid, age, pdb), path)
        return path
    sys.exit("no RSDS record")


def symbols(pe, pdb, out):
    base = pe.OPTIONAL_HEADER.ImageBase
    secs = {i + 1: base + s.VirtualAddress for i, s in enumerate(pe.sections)}
    txt = subprocess.run(["llvm-pdbutil", "dump", "--publics", pdb], capture_output=True,
                         text=True, check=True).stdout
    syms, name = [], None
    for line in txt.splitlines():
        m = re.search(r"S_PUB32 \[size = \d+\] `(.*)`", line)
        if m:
            name = m.group(1)
            continue
        m = re.search(r"addr = (\d+):(\d+)", line)
        if m and name and int(m.group(1)) in secs:
            syms.append((secs[int(m.group(1))] + int(m.group(2)), name))
            name = None
    syms.sort()
    with open(os.path.join(out, "syms.txt"), "w") as f:
        for a, n in syms:
            f.write("%x %s\n" % (a, n))
    return syms


def disassemble(binary, syms, out):
    addrs = [a for a, _ in syms]

    def name(a):
        i = bisect.bisect_right(addrs, a) - 1
        if i < 0:
            return "?"
        off = a - addrs[i]
        return syms[i][1] + ("+0x%x" % off if off else "")

    cmd = ["llvm-objdump", "-d", "--no-show-raw-insn", binary]
    if pefile.PE(binary, fast_load=True).FILE_HEADER.Machine == 0x8664:
        cmd[2:2] = ["-M", "intel"]
    dis = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    with open(os.path.join(out, "dis.txt"), "w") as f:
        for line in dis.splitlines():
            m = re.match(r"\s*([0-9a-f]+):\s*(.*)", line)
            if not m:
                continue
            a, ins = int(m.group(1), 16), m.group(2)
            i = bisect.bisect_left(addrs, a)
            if i < len(addrs) and addrs[i] == a:
                f.write("\n<%s>\n" % syms[i][1])
            ins = re.sub(r"<[^>]*>", "", ins)     # objdump's own export-based guesses
            ins = re.sub(r"0x([0-9a-f]{9,16})\b",
                         lambda mm: "0x%s <%s>" % (mm.group(1), name(int(mm.group(1), 16)))
                         if addrs and int(mm.group(1), 16) >= addrs[0] else mm.group(0), ins)
            f.write("%x: %s\n" % (a, ins))


def strings(pe, out):
    base = pe.OPTIONAL_HEADER.ImageBase
    with open(os.path.join(out, "strings.txt"), "w") as f:
        for s in pe.sections:
            for m in re.finditer(rb"(?:[\x20-\x7e]\x00){12,}", s.get_data()):
                f.write("%x %s\n" % (base + s.VirtualAddress + m.start(), m.group().decode("utf-16-le")))


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    if sys.argv[2] == "--winbindex" and len(sys.argv) == 5:
        binary = from_winbindex(sys.argv[3], sys.argv[4], out)
    elif sys.argv[2] == "--local":
        binary = sys.argv[3]
    else:
        sys.exit(__doc__)
    pe = pefile.PE(binary)
    pdb = fetch_pdb(pe, out)
    syms = symbols(pe, pdb, out)
    disassemble(binary, syms, out)
    strings(pe, out)
    print("%s: %d symbols -> %s/{syms,dis,strings}.txt" % (binary, len(syms), out))


if __name__ == "__main__":
    main()
