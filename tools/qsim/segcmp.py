#!/usr/bin/env python3
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

"""segcmp.py <elfA> <elfB> -- are the loaded images (PT_LOAD contents + entry) identical?"""
import sys, hashlib
from elftools.elf.elffile import ELFFile
def img(p):
    e = ELFFile(open(p, "rb"))
    h = hashlib.sha256()
    segs = []
    for s in e.iter_segments():
        if s["p_type"] != "PT_LOAD" or s["p_memsz"] == 0:
            continue
        d = s.data()
        segs.append((s["p_vaddr"], s["p_memsz"], len(d)))
        h.update(s["p_vaddr"].to_bytes(4, "little") + s["p_memsz"].to_bytes(4, "little") + d)
    h.update(e["e_entry"].to_bytes(4, "little"))
    return h.hexdigest(), segs
a, b = img(sys.argv[1]), img(sys.argv[2])
print(sys.argv[1], a[0], [(hex(v), hex(m)) for v, m, _ in a[1]])
print(sys.argv[2], b[0], [(hex(v), hex(m)) for v, m, _ in b[1]])
print("IDENTICAL loaded image" if a[0] == b[0] else "DIFFERENT loaded image")
sys.exit(0 if a[0] == b[0] else 1)
