#!/usr/bin/env python3
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

"""Verify that every stub key instruction assembled to the intended encoding
(objdump -M no-aliases text), so compressed/32-bit forms are really exercised."""
import sys, re

dis, keys = sys.argv[1], sys.argv[2]
want = {}
for line in open(keys):
    line = line.rstrip("\n")
    if not line: continue
    sym, exp = line.split("\t", 1)
    want[sym] = exp

got = {}
cur = None
for line in open(dis):
    m = re.match(r"^[0-9a-f]+ <(stub\d+_key)>:", line)
    if m:
        cur = m.group(1)
        continue
    if cur:
        m = re.match(r"^\s*[0-9a-f]+:\s+([0-9a-f]+)\s+(.*)$", line)
        if m:
            got[cur] = (m.group(1), m.group(2).split("#")[0].strip())
            cur = None

bad = 0
checked = 0
for sym, exp in want.items():
    if exp == "-": continue
    if sym not in got:
        print("missing key", sym); bad += 1; continue
    enc, txt = got[sym]
    txt = re.sub(r"\s+", "\t", txt, count=1)
    ok = txt.startswith(exp) if exp.endswith(",") or exp.endswith("\t") else txt == exp
    # compressed mnemonics must be 16-bit encodings, others 32-bit
    if exp.startswith("c.") and len(enc) != 4: ok = False
    if not exp.startswith("c.") and len(enc) != 8: ok = False
    checked += 1
    if not ok:
        bad += 1
        if bad < 40: print("MISMATCH %s: expected '%s' got '%s' [%s]" % (sym, exp, txt, enc))
print("key encodings: %d checked, %d mismatches" % (checked, bad))
sys.exit(1 if bad else 0)
