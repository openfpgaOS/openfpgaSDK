#!/usr/bin/env python3
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

"""addr2sym.py <elf> <addr>... -- map addresses to function+offset (pyelftools)."""
import sys, bisect
from elftools.elf.elffile import ELFFile
elf = ELFFile(open(sys.argv[1], 'rb'))
syms = []
for sec in elf.iter_sections():
    if sec.header.sh_type != 'SHT_SYMTAB': continue
    for s in sec.iter_symbols():
        if s['st_info']['type'] in ('STT_FUNC', 'STT_OBJECT') and s['st_value']:
            syms.append((s['st_value'], s['st_size'], s.name, s['st_info']['type']))
syms.sort()
keys = [s[0] for s in syms]
for a in sys.argv[2:]:
    v = int(a, 16)
    i = bisect.bisect_right(keys, v) - 1
    best = None
    while i >= 0 and syms[i][0] > v - 0x100000:
        if syms[i][0] <= v < syms[i][0] + max(syms[i][1], 1):
            best = syms[i]; break
        i -= 1
    print(f"{a}: " + (f"{best[2]}+0x{v-best[0]:x} ({best[3]})" if best else "?"))
