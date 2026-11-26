# SPDX-License-Identifier: GPL-2.0
#
# opencl.yml: bindgen for rusticl, with C enums typed as on Linux.
#
# For an enum with no fixed type, the MSVC ABI makes the type int; GCC and
# clang elsewhere make it unsigned int when no enumerator is negative.
# rusticl is written against the second (for example
# `#[repr(u32)] enum RWFlags { RD = pipe_map_flags::PIPE_MAP_READ.0 }`), so
# the Windows bindings do not compile (run 45763641107). Both are 4 bytes and
# every value involved is non-negative, so this runs the real bindgen and
# then types each such enum as c_uint in the output: the newtype of a
# bitfield enum, the Type of a constified module, the alias of a consts
# enum, and the repr of a Rust enum. Enums with a negative value stay int.
#
# Used as meson's bindgen (native.ini): python bindgen-unsigned-enums.py ARGS
import os
import re
import subprocess
import sys

REAL = os.environ.get('BINDGEN_REAL', 'C:/cldeps/bin/bindgen.exe')

args = sys.argv[1:]
r = subprocess.run([REAL] + args)
if r.returncode or not any(a in ('-o', '--output') or a.startswith('--output=') for a in args):
    sys.exit(r.returncode)

out = None
for i, a in enumerate(args):
    if a in ('-o', '--output'):
        out = args[i + 1]
    elif a.startswith('--output='):
        out = a.split('=', 1)[1]

src = open(out, encoding='utf-8').read()
INT = r'(?:::std::os::raw::|::core::ffi::|std::os::raw::|core::ffi::)?c_int'
n = 0


def negative(body):
    return re.search(r'=\s*-\s*\d', body) is not None


# 1. Bitfield and newtype enums: struct X(pub c_int); impl X { pub const A: X = X(..); }
for m in list(re.finditer(r'pub struct (\w+)\(pub ' + INT + r'\);', src)):
    name = m.group(1)
    consts = re.findall(r'pub const \w+: ' + name + r'\s*=\s*' + name + r'\((-?\d+)\)', src)
    if consts and all(not c.startswith('-') for c in consts):
        src = src.replace(m.group(0), 'pub struct %s(pub ::std::os::raw::c_uint);' % name, 1)
        n += 1

# 2. Constified modules: pub mod X { pub type Type = c_int; pub const A: Type = 1; }
def fix_mod(m):
    global n
    body = m.group(0)
    if 'pub const' in body and not negative(body):
        n += 1
        return re.sub(r'pub type Type = ' + INT + ';', 'pub type Type = ::std::os::raw::c_uint;', body, 1)
    return body


src = re.sub(r'pub mod \w+ \{\s*pub type Type = ' + INT + r';[^}]*\}', fix_mod, src)

# 3. Consts enums: pub type X = c_int; pub const X_A: X = 1;
for m in list(re.finditer(r'pub type (\w+) = ' + INT + r';', src)):
    name = m.group(1)
    consts = re.findall(r'pub const \w+: ' + name + r'\s*=\s*(-?\d+);', src)
    if consts and all(not c.startswith('-') for c in consts):
        src = src.replace(m.group(0), 'pub type %s = ::std::os::raw::c_uint;' % name, 1)
        n += 1

# 4. Rustified enums: #[repr(i32)] pub enum X { A = 1, }
def fix_enum(m):
    global n
    body = m.group(0)
    if not negative(body):
        n += 1
        return body.replace('#[repr(i32)]', '#[repr(u32)]', 1)
    return body


src = re.sub(r'#\[repr\(i32\)\](?:\s*#\[[^\]]*\])*\s*pub enum \w+ \{[^}]*\}', fix_enum, src)

open(out, 'w', encoding='utf-8', newline='\n').write(src)
print('%s: %d enum(s) typed unsigned' % (os.path.basename(out), n), file=sys.stderr)
