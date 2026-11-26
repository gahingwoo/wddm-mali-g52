# SPDX-License-Identifier: GPL-2.0
#
# opencl.yml: make meson pass each native library to rustc once.
#
# For a Rust static library (rlib) meson turns every .lib of the C
# dependencies into "-lstatic:-bundle=NAME", once per dependency that
# brings it. rusticl's rlibs get LLVM's and clang's static libraries
# through more than one dependency, and rustc refuses a library named twice
# with modifiers ("overriding linking modifiers from command line is not
# supported"). lld-link does not care about the order of .lib files, so the
# first mention is enough.
import pathlib
import sys

import mesonbuild.backend.ninjabackend as nb

p = pathlib.Path(nb.__file__)
s = p.read_text()
old = "            args.append(f'-l{type_}={libname}')\n"
new = ("            if f'-l{type_}={libname}' not in args:\n"
       "                args.append(f'-l{type_}={libname}')\n")
if new in s:
    print(f'{p}: already patched')
    sys.exit(0)
if s.count(old) != 1:
    sys.exit(f'{p}: the -l line of _link_library is not there once; meson changed')
p.write_text(s.replace(old, new))
print(f'{p}: rustc -l arguments deduplicated')
