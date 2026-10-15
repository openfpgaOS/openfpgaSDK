#!/usr/bin/env python3
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

"""coverage.py --debug-elf ELF_G [options] RUN_DIR... -- source-function coverage of qsim runs.

Every RUN_DIR must hold coverage.tsv (written by `qsim --coverage`) from runs of an
ELF whose loaded image is byte-identical to ELF_G.  ELF_G is the same build with -g
(apps/<app>/noise/build_variant.sh g for Quake): GCC guarantees -g does not change code, and coverage.py
checks the PT_LOAD images match, so the DWARF of ELF_G describes the production ELF.

Because the production build is LTO -O3, most functions exist only as inlined copies.
DWARF records every copy (DW_TAG_inlined_subroutine) with its address ranges, so a
source function counts as executed when ANY instruction inside ANY of its copies
(out-of-line or inlined, callees inlined into it included) executed in any run.

Output (stdout, or --out DIR for coverage.txt + coverage_functions.tsv + coverage_lines.tsv):
  * per source-file summary (functions with code, executed, code bytes covered);
  * RENDER functions with 0 executed instructions across all runs, by code size;
  * render functions with code but less than --partial % of their bytes executed;
  * render functions that have no code in the ELF (dead-stripped or folded away);
  * with --lines: never-executed source line ranges inside executed render functions.
"Render" files are the ones a rendering-equivalence gate must cover; the default
regex matches the Quake port's renderer (r_*.c d_*.c draw.c screen.c view.c vid_of.c
sbar.c chase.c of_emit.h).  Any other app passes its own --render-regex.
"""
import argparse
import bisect
import collections
import hashlib
import os
import re
import sys

from elftools.dwarf.ranges import BaseAddressEntry, RangeEntry
from elftools.elf.elffile import ELFFile

# Default --render-regex: the Quake port's renderer.  Override per app.
RENDER_RE = r"(^|/)(r_[a-z0-9_]+\.c|d_[a-z0-9_]+\.c|draw\.c|screen\.c|view\.c|vid_of\.c|sbar\.c|chase\.c|of_emit\.h)$"


def load_image_hash(path):
    e = ELFFile(open(path, "rb"))
    h = hashlib.sha256()
    for s in e.iter_segments():
        if s["p_type"] == "PT_LOAD" and s["p_memsz"]:
            h.update(s["p_vaddr"].to_bytes(4, "little") + s["p_memsz"].to_bytes(4, "little") + s.data())
    h.update(e["e_entry"].to_bytes(4, "little"))
    return h.hexdigest()


def die_ranges(dwarf, rl, cu, die):
    a = die.attributes
    if "DW_AT_low_pc" in a and "DW_AT_high_pc" in a:
        lo = a["DW_AT_low_pc"].value
        hi = a["DW_AT_high_pc"].value
        if a["DW_AT_high_pc"].form != "DW_FORM_addr":
            hi = lo + hi
        return [(lo, hi)] if hi > lo else []
    if "DW_AT_ranges" in a:
        top = cu.get_top_DIE()
        base = top.attributes["DW_AT_low_pc"].value if "DW_AT_low_pc" in top.attributes else 0
        out = []
        for ent in rl.get_range_list_at_offset(a["DW_AT_ranges"].value, cu=cu):
            if isinstance(ent, BaseAddressEntry):
                base = ent.base_address
            elif isinstance(ent, RangeEntry):
                lo = ent.begin_offset if ent.is_absolute else base + ent.begin_offset
                hi = ent.end_offset if ent.is_absolute else base + ent.end_offset
                if hi > lo:
                    out.append((lo, hi))
        return out
    return []


def cu_files(dwarf, cu):
    lp = dwarf.line_program_for_CU(cu)
    if lp is None:
        return [], None
    names = []
    ver = lp.header.version
    dirs = [d if isinstance(d, (bytes, str)) else d.DW_LNCT_path for d in lp.header.include_directory]
    for fe in lp.header.file_entry:
        nm = fe.name if hasattr(fe, "name") else fe.DW_LNCT_path
        di = fe.dir_index if hasattr(fe, "dir_index") else fe.DW_LNCT_directory_index
        nm = nm.decode(errors="replace") if isinstance(nm, bytes) else str(nm)
        dn = ""
        if ver >= 5 and di < len(dirs):
            dn = dirs[di]
        elif ver < 5 and 0 < di <= len(dirs):
            dn = dirs[di - 1]
        dn = dn.decode(errors="replace") if isinstance(dn, bytes) else str(dn)
        names.append(nm if nm.startswith("/") or not dn else nm)
    return names, lp


def norm_file(f):
    f = f.replace("\\", "/")
    m = re.search(r"(engine/[^/]+|sdk/[^ ]+|musl/[^ ]+|libgcc/[^ ]+)$", f)
    return m.group(1) if m else os.path.basename(f)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+", help="qsim output directories with coverage.tsv")
    ap.add_argument("--debug-elf", required=True, help="the -g build (loaded image identical to the runs' ELF)")
    ap.add_argument("--render-regex", default=RENDER_RE)
    ap.add_argument("--partial", type=float, default=50.0, help="list render functions below this %% of bytes executed")
    ap.add_argument("--lines", action="store_true", help="list never-executed line ranges in executed render functions")
    ap.add_argument("--out", help="write coverage.txt, coverage_functions.tsv, coverage_lines.tsv here")
    ap.add_argument("--no-image-check", action="store_true")
    ap.add_argument("--run-elf", action="append", help="the runs' ELF, if summary.json paths do not resolve")
    args = ap.parse_args()

    # executed pcs
    pcs = collections.Counter()
    shas = set()
    per_run = {}
    for r in args.runs:
        p = os.path.join(r, "coverage.tsv")
        s = set()
        with open(p) as f:
            for ln in f:
                if ln.startswith("#"):
                    m = re.search(r"elf_sha256=([0-9a-f]+)", ln)
                    if m:
                        shas.add(m.group(1))
                    continue
                if ln.startswith("pc"):
                    continue
                a, c = ln.split()
                pc = int(a, 16)
                pcs[pc] += int(c)
                s.add(pc)
        per_run[r] = s
    exec_sorted = sorted(pcs)

    elf_img = load_image_hash(args.debug_elf)
    if not args.no_image_check:
        import json
        checked = {}
        for r in args.runs:
            sj = json.load(open(os.path.join(r, "summary.json")))
            cands = [sj.get("elf_abspath"), sj.get("elf"), os.path.join(r, sj.get("elf", ""))] + (args.run_elf or [])
            ok = None
            for c in cands:
                if not c or not os.path.isfile(c):
                    continue
                if hashlib.sha256(open(c, "rb").read()).hexdigest() != sj["elf_sha256"]:
                    continue
                if c not in checked:
                    checked[c] = load_image_hash(c) == elf_img
                ok = checked[c]
                break
            if ok is None:
                sys.exit(f"coverage.py: {r}: cannot find the run's ELF (sha256 {sj['elf_sha256'][:16]}...); pass --run-elf")
            if not ok:
                sys.exit(f"coverage.py: {r}: loaded image of the run's ELF differs from {args.debug_elf}")

    e = ELFFile(open(args.debug_elf, "rb"))
    segs = [(s["p_vaddr"], s.data()) for s in e.iter_segments() if s["p_type"] == "PT_LOAD" and s["p_filesz"]]

    def insn_len(pc):
        for va, data in segs:
            if va <= pc < va + len(data):
                return 4 if (data[pc - va] & 3) == 3 else 2
        return 2

    dwarf = e.get_dwarf_info()
    rl = dwarf.range_lists()
    render_re = re.compile(args.render_regex)

    # abstract function identity: (file, name) from the DIE that carries DW_AT_name
    def ident(die):
        seen = 0
        d = die
        while d is not None and seen < 8:
            a = d.attributes
            if "DW_AT_name" in a:
                nm = a["DW_AT_name"].value.decode(errors="replace")
                f = "?"
                if "DW_AT_decl_file" in a:
                    files, _ = cu_files_cache(d.cu)
                    k = a["DW_AT_decl_file"].value
                    if 0 <= k < len(files):
                        f = norm_file(files[k])
                return f, nm
            nxt = None
            for key in ("DW_AT_abstract_origin", "DW_AT_specification"):
                if key in a:
                    nxt = d.get_DIE_from_attribute(key)
                    break
            d = nxt
            seen += 1
        return None

    _fc = {}

    def cu_files_cache(cu):
        k = cu.cu_offset
        if k not in _fc:
            _fc[k] = cu_files(dwarf, cu)
        return _fc[k]

    funcs = collections.defaultdict(lambda: {"ranges": [], "instances": 0, "inlined": 0, "declared": False})
    line_rows = []   # (addr, file, line) from every CU line program
    for cu in dwarf.iter_CUs():
        files, lp = cu_files_cache(cu)
        if lp is not None:
            for ent in lp.get_entries():
                st = ent.state
                if st is None:
                    continue
                if 0 <= st.file < len(files):
                    line_rows.append((st.address, norm_file(files[st.file]), st.line, st.end_sequence))
        for die in cu.iter_DIEs():
            if die.tag not in ("DW_TAG_subprogram", "DW_TAG_inlined_subroutine"):
                continue
            idt = ident(die)
            if idt is None:
                continue
            fn = funcs[idt]
            if die.tag == "DW_TAG_subprogram" and "DW_AT_declaration" not in die.attributes:
                fn["declared"] = True
            rg = die_ranges(dwarf, rl, cu, die)
            if rg:
                fn["ranges"].extend(rg)
                fn["instances"] += 1
                if die.tag == "DW_TAG_inlined_subroutine":
                    fn["inlined"] += 1

    def merge(rs):
        rs = sorted(rs)
        out = []
        for lo, hi in rs:
            if out and lo <= out[-1][1]:
                out[-1][1] = max(out[-1][1], hi)
            else:
                out.append([lo, hi])
        return out

    rows = []
    for (f, nm), fn in funcs.items():
        rs = merge(fn["ranges"])
        size = sum(hi - lo for lo, hi in rs)
        ex_pcs = 0
        ex_cnt = 0
        ex_bytes = 0
        for lo, hi in rs:
            i = bisect.bisect_left(exec_sorted, lo)
            j = bisect.bisect_left(exec_sorted, hi)
            ex_pcs += j - i
            for k in range(i, j):
                ex_cnt += pcs[exec_sorted[k]]
        rows.append({"file": f, "name": nm, "size": size, "instances": fn["instances"], "inlined": fn["inlined"],
                     "exec_pcs": ex_pcs, "exec_count": ex_cnt, "ranges": rs, "declared": fn["declared"]})

    # line table -> executed / not executed lines (by address interval)
    line_rows.sort()
    starts = [r[0] for r in line_rows]
    line_code = collections.defaultdict(int)     # (file, line) -> bytes
    line_exec = collections.defaultdict(int)     # (file, line) -> executed pcs
    for i, (addr, f, ln, endseq) in enumerate(line_rows):
        if endseq or i + 1 >= len(line_rows):
            continue
        nxt = line_rows[i + 1][0]
        if nxt <= addr or nxt - addr > 0x10000:
            continue
        line_code[(f, ln)] += nxt - addr
        a = bisect.bisect_left(exec_sorted, addr)
        b = bisect.bisect_left(exec_sorted, nxt)
        if b > a:
            line_exec[(f, ln)] += b - a

    out_txt = []
    P = out_txt.append
    P(f"qsim coverage: {len(args.runs)} runs, {len(pcs)} distinct executed pcs, debug ELF {args.debug_elf}")
    P("runs: " + " ".join(os.path.basename(os.path.normpath(r)) for r in args.runs))
    P(f"ELF sha256 (runs): {', '.join(sorted(shas))}; loaded image {elf_img[:16]}... "
      + ("(checked identical)" if not args.no_image_check else "(not checked)"))
    P("")
    by_file = collections.defaultdict(list)
    for r in rows:
        by_file[r["file"]].append(r)
    P("== per engine source file: functions with code (executed/total), source lines with code (executed/total) ==")
    lines_by_file = collections.defaultdict(lambda: [0, 0])
    for (f, ln), nb in line_code.items():
        lines_by_file[f][1] += 1
        if line_exec.get((f, ln)):
            lines_by_file[f][0] += 1
    for f in sorted(by_file):
        if "libgcc" in f or "musl" in f or f == "?" or re.match(r"^[a-z]+[dts]f[0-9]?\.c$", os.path.basename(f)):
            continue
        rs = [r for r in by_file[f] if r["size"]]
        if not rs:
            continue
        ex = [r for r in rs if r["exec_pcs"]]
        le, lt = lines_by_file.get(f, [0, 0])
        P(f"  {f:<22} functions {len(ex):>4}/{len(rs):<4} ({100.0 * len(ex) / len(rs):5.1f}%)  "
          f"lines {le:>5}/{lt:<5} ({(100.0 * le / lt) if lt else 0:5.1f}%)  {'render' if render_re.search(f) else ''}")
    P("")
    rend = [r for r in rows if render_re.search(r["file"])]
    zero = sorted([r for r in rend if r["size"] and not r["exec_pcs"]], key=lambda r: (-r["size"], r["file"], r["name"]))
    P(f"== RENDER functions with 0 executed instructions across all runs ({len(zero)}) ==")
    P(f"  {'function':<34} {'file':<22} {'code bytes':>10} {'copies':>6} {'inlined':>7}")
    for r in zero:
        P(f"  {r['name']:<34} {r['file']:<22} {r['size']:>10} {r['instances']:>6} {r['inlined']:>7}")
    P("")
    part = []
    for r in rend:
        if not r["size"] or not r["exec_pcs"]:
            continue
        cov = 0
        for lo, hi in r["ranges"]:
            i = bisect.bisect_left(exec_sorted, lo)
            j = bisect.bisect_left(exec_sorted, hi)
            for k in range(i, j):
                cov += insn_len(exec_sorted[k])
        pctv = 100.0 * min(1.0, cov / r["size"]) if r["size"] else 0.0
        if pctv < args.partial:
            part.append((pctv, r))
    part.sort(key=lambda x: (x[0], -x[1]["size"]))
    P(f"== render functions with code executed but below {args.partial:.0f}% of their code bytes executed ({len(part)}) ==")
    for pctv, r in part:
        P(f"  {r['name']:<34} {r['file']:<22} {r['size']:>10} bytes  {pctv:5.1f}% executed  ({r['exec_pcs']} pcs)")
    P("")
    nocode = sorted([r for r in rend if not r["size"] and r["declared"]], key=lambda r: (r["file"], r["name"]))
    P(f"== render functions defined in the source with no code in the ELF (unused or folded away) ({len(nocode)}) ==")
    for i in range(0, len(nocode), 4):
        P("  " + "  ".join(f"{r['name'] + ' (' + r['file'].split('/')[-1] + ')':<44}" for r in nocode[i:i + 4]))
    if args.lines:
        P("")
        P("== never-executed source lines inside executed render functions (file: line ranges) ==")
        per_file = collections.defaultdict(list)
        for (f, ln), nb in line_code.items():
            if render_re.search(f) and not line_exec.get((f, ln)):
                per_file[f].append(ln)
        for f in sorted(per_file):
            ls = sorted(per_file[f])
            spans = []
            for ln in ls:
                if spans and ln <= spans[-1][1] + 1:
                    spans[-1][1] = ln
                else:
                    spans.append([ln, ln])
            P(f"  {f}: " + ", ".join(f"{a}" if a == b else f"{a}-{b}" for a, b in spans))
    text = "\n".join(out_txt) + "\n"
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        open(os.path.join(args.out, "coverage.txt"), "w").write(text)
        with open(os.path.join(args.out, "coverage_functions.tsv"), "w") as fo:
            fo.write("file\tfunction\tcode_bytes\tcopies\tinlined_copies\texecuted_pcs\texecutions\trender\n")
            for r in sorted(rows, key=lambda r: (r["file"], r["name"])):
                if r["size"]:
                    fo.write(f"{r['file']}\t{r['name']}\t{r['size']}\t{r['instances']}\t{r['inlined']}\t"
                             f"{r['exec_pcs']}\t{r['exec_count']}\t{1 if render_re.search(r['file']) else 0}\n")
        with open(os.path.join(args.out, "coverage_lines.tsv"), "w") as fo:
            fo.write("file\tline\tcode_bytes\texecuted_pcs\n")
            for (f, ln) in sorted(line_code):
                fo.write(f"{f}\t{ln}\t{line_code[(f, ln)]}\t{line_exec.get((f, ln), 0)}\n")
    sys.stdout.write(text)


if __name__ == "__main__":
    main()
