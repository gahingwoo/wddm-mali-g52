# SPDX-License-Identifier: GPL-2.0
import json, sys
a, b = json.load(open(sys.argv[1])), json.load(open(sys.argv[2]))
for k in a:
    print("%-15s %08x -> %08x%s" % (k, a[k], b[k], "   <--" if a[k] != b[k] else ""))
