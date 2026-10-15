#!/usr/bin/env python3
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

"""compare.py <outA> <outB>        -- A/B comparison of two qsim output directories.
compare.py [--gate] <gateA> <gateB> -- equivalence gate over two gate.sh matrix directories.

Single run pair (<outX> holds frames.csv) reports
  * frame alignment: the timedemo window is matched by position and every other
    frame by (host_framecount, n-th flip in that host frame), so an extra or
    missing boot/loading flip does not shift every later frame;
  * per-frame oracle equality (hash_px, hash_z, hash_pal, hash_cmd separately) on
    the aligned frames, first divergent frame and number of divergent frames;
  * frames that exist in only one run;
  * memory-coherency hazards (runs made with --mem lint|strict) per class;
  * workload parity (the app profile's `parity` facts, caps.heap_size, result line);
  * total and per-frame percentile deltas of insns / est_cycles / cache misses /
    mispredicts / soft-float calls (all frames and the measurement window), with
    the layout noise band of est_cycles when the app's noise_floor.json has the workload;
  * top per-function deltas from profile.tsv (when both runs used --profile).

Gate (<gateX> holds one run directory per workload and mode, as written by
gate.sh: <workload>.<mode>/) prints one line per run pair, schedule/memory-mode
consistency per ELF and a final "GATE RESULT: PASS" or "GATE RESULT: FAIL: <reasons>".
Reference races: in a mode other than eager-lint, a frame that A itself renders
differently than in its eager-lint run of the same workload is a race (or an
uninitialized-memory read, in eager-seed) in the reference.  Its content is not an
oracle, so that frame and its hazards are excluded from that mode's A/B comparison
and listed; eager-lint still compares it.

Exit status: 1 when, in any compared run pair,
  * hash_px, hash_z or hash_pal differ on an aligned frame;
  * a frame exists in only one run (--allow-boot-flips: allowed for frames of the
    first host frame, i.e. console/loading flips before the game runs);
  * a run did not finish the same way (status differs, or error);
  * the workload parity differs: caps.heap_size or one of the app profile's `parity` facts
    size (pin --heap-size to compare ELFs whose .bss differs; gate.sh does);
  * B has more cache-aware memory-coherency hazards than A (both runs linted);
  * (gate) a run is missing on one side.
0 otherwise; 2 on usage errors.
hash_cmd is informational: command words embed ELF-dependent addresses, so it
is only expected to match between runs of the same ELF.
A single run pair checks one workload, GPU schedule and memory mode only; the
equivalence gate is gate.sh.
"""
import argparse
import csv
import difflib
import json
import math
import os
import re
import sys

METRICS = ["insns", "est_cycles", "icache_miss_sdram", "icache_miss_bram", "dcache_miss",
           "mispredict", "softfloat_calls", "gpu_cmd_words", "gpu_pixels"]
EXTRA_COLS = ["mem_hazards", "gpu_barriers", "gpu_wait_px"]     # newer qsim; 0 when absent
HASHES = ["hash_px", "hash_z", "hash_pal", "hash_cmd"]
ORACLE = ["hash_px", "hash_z", "hash_pal"]
HERE = os.path.dirname(os.path.abspath(__file__))
# Layout noise floor of the app under test: apps/<app>/noise/noise_floor.json,
# written by noise_floor.py.  QSIM_APP selects the app, as it does for run.sh
# and gate.sh; --noise overrides the path outright.
NOISE_DEFAULT = os.path.join(HERE, "apps", os.environ.get("QSIM_APP", "quake"),
                             "noise", "noise_floor.json")


def load_run(d):
    rows = []
    with open(os.path.join(d, "frames.csv")) as f:
        for r in csv.DictReader(f):
            for k in METRICS + ["frame", "host_framecount", "buffer"]:
                r[k] = int(r[k])
            for k in EXTRA_COLS:
                r[k] = int(r.get(k) or 0)
            rows.append(r)
    summ = json.load(open(os.path.join(d, "summary.json")))
    prof = None
    p = os.path.join(d, "profile.tsv")
    if os.path.exists(p):
        prof = {}
        with open(p) as f:
            for r in csv.DictReader(f, delimiter="\t"):
                for k in ("self_insns", "self_cycles", "icache_miss", "dcache_miss", "mispredict", "calls",
                          "tailcalls"):
                    r[k] = int(r[k])
                prof[r["function"]] = r
    con = ""
    p = os.path.join(d, "console.log")
    if os.path.exists(p):
        con = open(p, errors="replace").read()
    return rows, summ, prof, con


def pct(vals, p):
    if not vals:
        return 0
    s = sorted(vals)
    k = max(1, min(len(s), math.ceil(p * len(s))))
    return s[k - 1]


def fmt_delta(a, b):
    d = b - a
    rel = (100.0 * d / a) if a else (0.0 if d == 0 else float("inf"))
    return f"{a:>15,} {b:>15,} {d:>+14,} {rel:>+8.3f}%"


def parity_facts(s):
    """workload-parity facts qsim extracted with the app profile's `parity`
    rules, plus the profile's result line.  A run recorded before an app
    profile declared a rule simply has no entry for it; a key missing on one
    side is reported as unknown, never as a difference."""
    facts = dict(s.get("parity") or {})
    facts["result"] = s.get("timedemo_result") or None
    return facts


def run_mode(s):
    return (f"gpu_sched={s.get('gpu_sched', 'eager')} mem={s.get('mem_mode', 'flat')}"
            + (f" seed_mem={s['seed_mem']}" if s.get("seed_mem") else ""))


def workload_of(s):
    """workload label (qsim --label; run.sh and gate.sh always pass it)"""
    return s.get("workload") or None


def hazards(s):
    """cache-aware hazard total, or None when the run was not linted"""
    m = s.get("memory_coherency")
    return None if m is None else int(m["hazards"])


# Hazard COUNTS are layout noise once the reference has a hazard site of its own: the same broken
# loop touches a different number of cache lines when the compiler moves the stack (fx2 moves by up
# to +48% over the layout variants of the baseline).  The SITES (class, function) do not move, so
# they decide, and counts only fail when the sites table is incomplete.


def hazard_sites(s):
    """{(class, function): count} of cache-aware hazards, or None for runs without the field"""
    m = s.get("memory_coherency") or {}
    if "sites" not in m:
        return None
    out = {}
    for e in m["sites"]:
        if e["class"].startswith("wc_"):
            continue
        k = (e["class"], e["func"])
        out[k] = out.get(k, 0) + e["count"]
    return out


def hazard_classes(s):
    m = s.get("memory_coherency") or {}
    return {k: v for k, v in (m.get("classes") or {}).items() if not k.startswith("wc_")}


def hazard_reasons(sa, sb, ha, hb, where=""):
    """(failures, notes) for the memory-coherency comparison of one run pair.

    A hazard CLASS the reference does not have at all is a real change and fails.  Where the
    reference already has that class, neither the count nor the set of sites is evidence: both
    move with code and data layout (fx2's water-warp bug moves by up to +48%, and which
    function's stack frame shares a cache line with the GPU-written buffer is an accident of
    the link).  Those are reported as notes, with the sites, so a human can look."""
    why, notes = [], []
    if ha is None or hb is None:
        return why, notes
    ca, cb = hazard_classes(sa), hazard_classes(sb)
    sita, sitb = hazard_sites(sa) or {}, hazard_sites(sb) or {}
    for cls in sorted(cb):
        if cb[cls] > 0 and ca.get(cls, 0) == 0:
            sites = [fn for (c, fn) in sorted(sitb) if c == cls]
            why.append(f"new memory-coherency hazard class {cls}: {cb[cls]} events{where}"
                       + (" in " + ", ".join(sites[:4]) + (f", ... ({len(sites)} functions)"
                                                           if len(sites) > 4 else "") if sites else ""))
        elif cb[cls] > ca.get(cls, 0):
            new = sorted(fn for (c, fn) in sitb if c == cls and (c, fn) not in sita)
            notes.append(f"{cls} {ca.get(cls, 0)} -> {cb[cls]} at a class the reference already has "
                         f"(counts and sites move with layout){where}"
                         + (f"; sites not in A: {', '.join(new[:4])}" if new else ""))
    if hb > ha and not why:
        notes.append(f"memory-coherency hazards {ha} -> {hb}{where}")
    return why, notes


# ---------------------------------------------------------------------------------------------
# frame alignment

def window_of(s, nrows):
    w = s.get("per_frame_timedemo_window") or {}
    kind = (s.get("window") or {}).get("kind") or ("timedemo" if s.get("timedemo_result") else "none")
    return kind, int(w.get("first_frame", nrows)), int(w.get("frames", 0))


def key_align(ra, rb, oa=0, ob=0):
    """Pair rows by host_framecount, then by flip order inside each host frame.  When a host
    frame has a different number of flips in the two runs (an added or dropped console/loading
    flip), the flips of that one host frame are matched by content instead, so the mismatch
    stays inside that host frame: everything it cannot match is reported as unmatched (which
    fails the comparison), never as a silent match."""
    def group(rows):
        g = {}
        for i, r in enumerate(rows):
            g.setdefault(r["host_framecount"], []).append(i)
        return g
    ga, gb = group(ra), group(rb)
    pairs, ua, ub = [], [], []
    for h in sorted(set(ga) | set(gb)):
        ia, ib = ga.get(h, []), gb.get(h, [])
        if len(ia) == len(ib):
            pairs += [(i + oa, j + ob) for i, j in zip(ia, ib)]
            continue
        ka = [tuple(ra[i][x] for x in ORACLE) for i in ia]
        kb = [tuple(rb[j][x] for x in ORACLE) for j in ib]
        sm = difflib.SequenceMatcher(a=ka, b=kb, autojunk=False)
        ma, mb = set(), set()
        for blk in sm.get_matching_blocks():
            for k in range(blk.size):
                pairs.append((ia[blk.a + k] + oa, ib[blk.b + k] + ob))
                ma.add(blk.a + k)
                mb.add(blk.b + k)
        if not ma:                      # nothing matches by content: pair positionally
            n = min(len(ia), len(ib))
            pairs += [(ia[k] + oa, ib[k] + ob) for k in range(n)]
            ma, mb = set(range(n)), set(range(n))
        ua += [ia[k] + oa for k in range(len(ia)) if k not in ma]
        ub += [ib[k] + ob for k in range(len(ib)) if k not in mb]
    pairs.sort()
    return pairs, sorted(ua), sorted(ub)


def align(ra, sa, rb, sb):
    """-> (pairs [(i, j)], rows only in A, rows only in B, method)"""
    ka, fa, na = window_of(sa, len(ra))
    kb, fb, nb = window_of(sb, len(rb))
    if ka == kb == "timedemo" and na == nb and na > 0 and fa + na == len(ra) and fb + nb == len(rb):
        pre, ua, ub = key_align(ra[:fa], rb[:fb])
        pairs = pre + [(fa + k, fb + k) for k in range(na)]
        method = "timedemo window by position; earlier frames by (host_framecount, n-th flip)"
    else:
        pairs, ua, ub = key_align(ra, rb)
        method = "(host_framecount, n-th flip in that host frame)"
    if any(pairs[k][1] <= pairs[k - 1][1] for k in range(1, len(pairs))):
        n = min(len(ra), len(rb))       # host_framecount went backwards: fall back to flip order
        pairs = [(i, i) for i in range(n)]
        ua, ub = list(range(n, len(ra))), list(range(n, len(rb)))
        method = "flip order (host_framecount is not monotonic)"
    return pairs, ua, ub, method


def oracle_diff(ra, rb, pairs):
    """{hash: [(i, j) aligned pairs whose hash differs]}"""
    return {h: [(i, j) for i, j in pairs if ra[i][h] != rb[j][h]] for h in HASHES}


def boot_rows(rows, idx):
    """indices among idx that belong to the first host frame of the run (console/loading flips)"""
    if not rows:
        return []
    h0 = rows[0]["host_framecount"]
    return [i for i in idx if rows[i]["host_framecount"] == h0]


def unmatched_reason(ra, rb, ua, ub, allow_boot, nlist):
    if not ua and not ub:
        return None, False
    boot_only = len(boot_rows(ra, ua)) == len(ua) and len(boot_rows(rb, ub)) == len(ub)
    txt = (f"frames in one run only: A {len(ua)}"
           + (f" (frames {' '.join(str(ra[i]['frame']) for i in ua[:nlist])}{' ...' if len(ua) > nlist else ''})" if ua else "")
           + f", B {len(ub)}"
           + (f" (frames {' '.join(str(rb[j]['frame']) for j in ub[:nlist])}{' ...' if len(ub) > nlist else ''})" if ub else "")
           + (" [all in the first host frame]" if boot_only else ""))
    return txt, (boot_only and allow_boot)


def parity_reasons(sa, ca, sb, cb):
    why = []
    if sa.get("heap_size") != sb.get("heap_size"):
        why.append(f"workload parity: caps.heap_size {sa.get('heap_size')} vs {sb.get('heap_size')} "
                   "(pin --heap-size; gate.sh pins it to ELF A's value)")
    fa, fb = parity_facts(sa), parity_facts(sb)
    for k in sorted(set(fa) & set(fb)):
        if fa[k] is not None and fb[k] is not None and fa[k] != fb[k]:
            why.append(f"workload parity: {k} {fa[k]} vs {fb[k]}")
    return why


def pair_failures(ra, sa, ca, rb, sb, cb, pairs, ua, ub, div, allow_boot=False, nlist=10, skip_hazards=False):
    """reasons the pair is not equivalent (empty list = equivalent), and notes"""
    why, notes = [], []
    for h in ORACLE:
        if div[h]:
            why.append(f"{h} differs on {len(div[h])} frames (first frame {ra[div[h][0][0]]['frame']})")
    txt, allowed = unmatched_reason(ra, rb, ua, ub, allow_boot, nlist)
    if txt:
        (notes if allowed else why).append(txt + (" (allowed: --allow-boot-flips)" if allowed else ""))
    if sa["status"] != sb["status"]:
        why.append(f"status {sa['status']} vs {sb['status']}")
    elif sa["status"] == "error":
        why.append("both runs ended in error")
    why += parity_reasons(sa, ca, sb, cb)
    if not skip_hazards:
        w, n = hazard_reasons(sa, sb, hazards(sa), hazards(sb))
        why += w
        notes += n
    return why, notes


def class_delta(sa, sb):
    ma, mb = sa.get("memory_coherency"), sb.get("memory_coherency")
    if not ma or not mb:
        return ""
    parts = [f"{k} {ma['classes'].get(k, 0)}->{v}" for k, v in mb["classes"].items()
             if v != ma["classes"].get(k, 0) and not k.startswith("wc_")]
    return "; ".join(parts)


def is_run_dir(d):
    return os.path.isfile(os.path.join(d, "frames.csv"))


def gate_runs(d):
    return sorted(e for e in os.listdir(d) if is_run_dir(os.path.join(d, e)))


REF_MODE = "eager-lint"


def hazard_rows(rows, s, skip):
    """cache-aware hazards outside the skipped frame rows (rows + events after the last flip)"""
    total = hazards(s)
    if total is None:
        return None
    in_rows = sum(r["mem_hazards"] for r in rows)
    return sum(r["mem_hazards"] for i, r in enumerate(rows) if i not in skip) + (total - in_rows)


# ---------------------------------------------------------------------------------------------
# layout noise floor (apps/<app>/noise/noise_floor.json, written by noise_floor.py)

def load_noise(path):
    if not path or not os.path.exists(path):
        return None
    try:
        return json.load(open(path))
    except (OSError, ValueError):
        return None


def noise_line(noise, wl, metric, a, b):
    if not noise or not wl or wl not in noise.get("workloads", {}):
        return None
    m = noise["workloads"][wl].get(metric)
    if not m or not a:
        return None
    rel = 100.0 * (b - a) / a
    band, band_all = m["band_pct"], m.get("band_all_pct", m["band_pct"])
    verdict = ("INSIDE the layout noise band" if abs(rel) <= band else
               "inside the wider alignment band" if abs(rel) <= band_all else "OUTSIDE both bands")
    return (f"  {metric:<18} {rel:+8.3f}%   noise band +-{band:.3f}% ({m['variants']} address-only layout "
            f"variants, sd {m['stdev_pct']:.3f}%), +-{band_all:.3f}% incl. alignment variants: {verdict}")


# ---------------------------------------------------------------------------------------------

def gate(a, b, nlist, allow_boot):
    try:
        names = sorted(set(gate_runs(a)) | set(gate_runs(b)))
    except OSError as e:
        print(f"compare.py: {e}", file=sys.stderr)
        return 2
    if not names:
        print("compare.py: no run directories (<workload>.<mode>/frames.csv) found", file=sys.stderr)
        return 2
    print(f"A: {a}\nB: {b}")
    fails = []
    runs = {"A": {}, "B": {}}
    for nm in names:
        for side, d in (("A", a), ("B", b)):
            p = os.path.join(d, nm)
            if not is_run_dir(p):
                continue
            try:
                rr, ss, _, cc = load_run(p)
                runs[side][nm] = (rr, ss, cc)
            except (OSError, ValueError, KeyError) as e:
                print(f"  {side} {nm}: cannot load: {e}")
                fails.append(f"{nm}: cannot load {side} ({e})")
    elf_a = next((s["elf_sha256"] for _, s, _ in runs["A"].values()), None)
    elf_b = next((s["elf_sha256"] for _, s, _ in runs["B"].values()), None)

    def ref_racy(nm):
        """row indices where A renders this workload differently than in its eager-lint run"""
        wl, _, mode = nm.partition(".")
        ref = runs["A"].get(f"{wl}.{REF_MODE}")
        if mode == REF_MODE or ref is None or nm not in runs["A"]:
            return set()
        (rr, sr, _), (rm, sm, _) = ref, runs["A"][nm]
        pairs, _, _, _ = align(rr, sr, rm, sm)
        return {j for i, j in pairs if any(rr[i][h] != rm[j][h] for h in ORACLE)}

    print(f"\n== per-run results ({len(names)} run pairs) ==")
    print(f"  {'run':<22} {'frames A/B':>11} {'px':>5} {'z':>5} {'pal':>5} {'cmd':>5} {'hazards A/B':>15} "
          f"{'barriers A/B':>15} {'ref-racy':>8}  result")
    for nm in names:
        if nm not in runs["A"] or nm not in runs["B"]:
            side = "A" if nm not in runs["A"] else "B"
            print(f"  {nm:<22} missing on {side}  FAIL")
            fails.append(f"{nm}: run missing on {side}")
            continue
        (ra, sa, ca), (rb, sb, cb) = runs["A"][nm], runs["B"][nm]
        if run_mode(sa) != run_mode(sb):
            fails.append(f"{nm}: modes differ ({run_mode(sa)} vs {run_mode(sb)})")
        pairs, ua, ub, method = align(ra, sa, rb, sb)
        skip = ref_racy(nm)
        # In a split-memory mode a coherency hazard decides what the frame renders, and its outcome
        # depends on where the compiler put the data, so a frame in which EITHER side hit a hazard is
        # not an oracle.  What must not change is the set of hazard SITES (class, function), which is
        # layout-independent and is compared below over the whole run.
        hazard_frames = set()
        if sa.get("mem_mode") == "strict" and sb.get("mem_mode") == "strict":
            bh = {j for j, r in enumerate(rb) if r["mem_hazards"]}
            hazard_frames = {i for i, r in enumerate(ra) if r["mem_hazards"]} | {i for i, j in pairs if j in bh}
        skip |= hazard_frames
        pairs_cmp = [(i, j) for i, j in pairs if i not in skip]
        div = oracle_diff(ra, rb, pairs_cmp)
        why, notes = pair_failures(ra, sa, ca, rb, sb, cb, pairs_cmp, ua, ub, div, allow_boot, nlist,
                                   skip_hazards=True)
        skip_b = {j for i, j in pairs if i in skip}
        ha, hb = hazard_rows(ra, sa, skip), hazard_rows(rb, sb, skip_b)
        w, n = hazard_reasons(sa, sb, ha, hb,
                              f" (outside {len(skip)} reference-racy frames)" if skip else "")
        why += w
        notes += n
        ba = sum(r["gpu_barriers"] for r in ra)
        bb = sum(r["gpu_barriers"] for r in rb)
        hz = f"{'-' if ha is None else ha}/{'-' if hb is None else hb}"
        print(f"  {nm:<22} {len(ra):>5}/{len(rb):<5} {len(div['hash_px']):>5} {len(div['hash_z']):>5} "
              f"{len(div['hash_pal']):>5} {len(div['hash_cmd']):>5} {hz:>15} {f'{ba}/{bb}':>15} {len(skip):>8}  "
              + ("PASS" if not why else "FAIL"))
        for w in why:
            print(f"      - {w}")
            fails.append(f"{nm}: {w}")
        for w in notes:
            print(f"      note: {w}")
        if ua or ub:
            print(f"      alignment: {method}")
        if skip:
            print(f"      reference-racy frames not compared in this mode (A renders them differently than in "
                  f"{REF_MODE}"
                  + (f", or one of the two hit a coherency hazard in it: {len(hazard_frames)} frames"
                     if hazard_frames else "")
                  + "): " + " ".join(str(ra[i]["frame"]) for i in sorted(skip)[:nlist])
                  + (" ..." if len(skip) > nlist else ""))
        cd = class_delta(sa, sb)
        if cd and hb is not None and ha is not None and hb != ha:
            print(f"      hazard classes (whole run): {cd}")
            ex = [e for e in sb["memory_coherency"].get("examples", []) if not e["class"].startswith("wc_")]
            for e in ex[:min(nlist, 4)]:
                print(f"        B e.g. {e['class']} frame {e['frame']} addr {e['addr']} pc {e['pc']} "
                      f"dirtied by {e['dirtied_by_pc']}")
        if any(div[h] for h in ORACLE) and nlist:
            for h in ORACLE:
                if div[h]:
                    print(f"      {h} frames: " + " ".join(str(ra[i]["frame"]) for i, _ in div[h][:nlist])
                          + (" ..." if len(div[h]) > nlist else ""))
        done = ("timedemo_complete", "workload_complete")
        if sa.get("status") not in done or sb.get("status") not in done:
            print(f"      status A={sa.get('status')} ({sa.get('stop_reason')}) B={sb.get('status')} "
                  f"({sb.get('stop_reason')})")

    print(f"\n== schedule / memory-mode consistency per ELF (px/z/pal vs the same workload's {REF_MODE} run) ==")
    for side in ("A", "B"):
        by_wl = {}
        for nm, rs in runs[side].items():
            wl, _, mode = nm.partition(".")
            by_wl.setdefault(wl, {})[mode] = rs
        lines = []
        for wl in sorted(by_wl):
            modes = by_wl[wl]
            if REF_MODE not in modes:
                continue
            rr, sr, _ = modes[REF_MODE]
            for mode in sorted(modes):
                if mode == REF_MODE:
                    continue
                rm, sm, _ = modes[mode]
                pairs, ua, ub, _ = align(rr, sr, rm, sm)
                d = oracle_diff(rr, rm, pairs)
                bad = sorted({i for h in ORACLE for i, _ in d[h]})
                if bad or ua or ub:
                    lines.append(f"  {side} {wl}: {mode}: {len(bad)} frames differ"
                                 + (f" (frames {' '.join(str(rr[i]['frame']) for i in bad[:nlist])}"
                                    + (" ..." if len(bad) > nlist else "") + ")" if bad else "")
                                 + (f", frame count {len(rr)} vs {len(rm)}" if ua or ub else ""))
        print("\n".join(lines) if lines else f"  {side}: every mode renders identically")
    print("  (a frame that differs for A is a race or an uninitialized-memory read in the reference: under that\n"
          "   mode it is excluded from the A/B comparison, which still covers it in " + REF_MODE + ")")
    sub = sorted({nm for side in runs for nm, (_, s, _) in runs[side].items()
                  if any((s.get("fpu_subnormal") or {}).values())})
    if sub:
        print(f"\nWARNING: FPU subnormal operands/results in {', '.join(sub)}: the os25 FPU ignores subnormals, "
              "qsim is exact IEEE, so the device may compute differently there")
    if elf_a and elf_a == elf_b:
        print("\nNOTE: A and B are the same ELF (determinism check)")

    print("\nGATE RESULT: " + ("PASS" if not fails else "FAIL: " + "; ".join(fails[:6])
                                + (f"; ... ({len(fails)} problems)" if len(fails) > 6 else "")))
    return 0 if not fails else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--gate", action="store_true",
                    help="compare gate.sh matrix directories (automatic when a/b hold no frames.csv)")
    ap.add_argument("--top", type=int, default=25, help="per-function delta rows (default 25)")
    ap.add_argument("--list", type=int, default=10, help="divergent frames listed per hash (default 10)")
    ap.add_argument("--allow-boot-flips", action="store_true",
                    help="frames of the first host frame (console/loading flips) may exist in one run only")
    ap.add_argument("--noise", default=NOISE_DEFAULT, help="layout noise floor JSON (default apps/$QSIM_APP/noise/noise_floor.json)")
    args = ap.parse_args()

    if args.gate or (not is_run_dir(args.a) and not is_run_dir(args.b)
                     and os.path.isdir(args.a) and os.path.isdir(args.b)):
        return gate(args.a, args.b, args.list, args.allow_boot_flips)

    try:
        ra, sa, pa, ca = load_run(args.a)
        rb, sb, pb, cb = load_run(args.b)
    except (OSError, ValueError, KeyError) as e:
        print(f"compare.py: cannot load runs: {e}", file=sys.stderr)
        return 2

    print(f"A: {args.a}\n   elf {sa['elf']}\n   sha256 {sa['elf_sha256']}  status={sa['status']}  frames={sa['frames']}"
          f"  {run_mode(sa)}  workload={workload_of(sa)}")
    print(f"B: {args.b}\n   elf {sb['elf']}\n   sha256 {sb['elf_sha256']}  status={sb['status']}  frames={sb['frames']}"
          f"  {run_mode(sb)}  workload={workload_of(sb)}")
    if sa["argv"] != sb["argv"]:
        print(f"WARNING: argv differs: {sa['argv']} vs {sb['argv']}")
    if run_mode(sa) != run_mode(sb):
        print("WARNING: the runs use different GPU schedules / memory modes")
    print("NOTE: one run pair checks one workload, schedule and memory mode; the equivalence gate is gate.sh")

    fa, fb = parity_facts(sa), parity_facts(sb)
    print("\n== workload parity ==")
    for k in sorted(set(fa) | set(fb)):
        known = fa.get(k) is not None and fb.get(k) is not None
        flag = "" if not known else ("" if fa[k] == fb[k] else "   <-- DIFFERS")
        print(f"  {k:<14} A={fa.get(k)!s:<40} B={fb.get(k)!s}{flag}")
    flag = "" if sa.get("heap_size") == sb.get("heap_size") else "   <-- DIFFERS"
    print(f"  {'heap_size':<14} A={sa['heap_size']!s:<40} B={sb['heap_size']}{flag}"
          + ("" if sa.get("heap_size_pinned") and sb.get("heap_size_pinned") else
             "   (not pinned in both runs: caps.heap_size follows each ELF's .bss)"))

    pairs, ua, ub, method = align(ra, sa, rb, sb)
    div = oracle_diff(ra, rb, pairs)
    fails, notes = pair_failures(ra, sa, ca, rb, sb, cb, pairs, ua, ub, div, args.allow_boot_flips, args.list)
    print(f"\n== per-frame oracle ({len(pairs)} aligned frames; A has {len(ra)}, B has {len(rb)}) ==")
    print(f"  alignment: {method}")
    for h in HASHES:
        d = div[h]
        state = "IDENTICAL" if not d and not ua and not ub else ("identical on aligned frames" if not d else "DIVERGES")
        line = f"  {h:<9} {state}"
        if d:
            i, j = d[0]
            line += (f": {len(d)} of {len(pairs)} frames differ; first divergent frame A {ra[i]['frame']} / "
                     f"B {rb[j]['frame']} (host_framecount {ra[i]['host_framecount']})")
        print(line)
        if d and args.list:
            print("            frames: " + " ".join(str(ra[i]["frame"]) for i, _ in d[:args.list])
                  + (" ..." if len(d) > args.list else ""))
    txt, _ = unmatched_reason(ra, rb, ua, ub, args.allow_boot_flips, args.list)
    print(f"  {txt}" if txt else "  every frame of each run has a partner in the other")
    if sa["oracle"]["hash_px_all"] == sb["oracle"]["hash_px_all"] and len(ra) == len(rb):
        print(f"  whole-run hash_px_all {sa['oracle']['hash_px_all']} (equal)")
    for n in notes:
        print(f"  note: {n}")

    for side, sx in (("A", sa), ("B", sb)):
        if any((sx.get("fpu_subnormal") or {}).values()):
            print(f"WARNING: {side} has FPU subnormal operands/results {sx['fpu_subnormal']} (os25 FPU ignores "
                  "subnormals; qsim is exact IEEE)")
    ha, hb = hazards(sa), hazards(sb)
    print("\n== memory coherency (cache-aware hazards; --mem lint|strict) ==")
    if ha is None or hb is None:
        print(f"  not checked: A {'linted' if ha is not None else 'not linted'}, "
              f"B {'linted' if hb is not None else 'not linted'} (run with --mem lint or --mem strict)")
    else:
        print(f"  total {ha} -> {hb}" + ("   <-- B HAS MORE HAZARDS" if hb > ha else ""))
        sita, sitb = hazard_sites(sa), hazard_sites(sb)
        if sita is not None and sitb is not None:
            new = sorted(k for k in sitb if k not in sita)
            print(f"  hazard sites (class, function): A {len(sita)}, B {len(sitb)}, new in B {len(new)}"
                  + (" <-- " + "; ".join(f"{c} in {f} ({sitb[(c, f)]})" for c, f in new[:4]) if new else ""))
        for k, v in sb["memory_coherency"]["classes"].items():
            va = sa["memory_coherency"]["classes"].get(k, 0)
            print(f"    {k:<44} {va:>12} {v:>12}" + ("   (worst case, advisory)" if k.startswith("wc_") else ""))
        if hb > ha:
            for e in sb["memory_coherency"].get("examples", []):
                if not e["class"].startswith("wc_"):
                    print(f"    B e.g. {e['class']} frame {e['frame']} addr {e['addr']} pc {e['pc']} "
                          f"dirtied by {e['dirtied_by_pc']}")

    have_model = bool(sa.get("profile")) and bool(sb.get("profile"))
    metrics = METRICS if have_model else ["insns", "gpu_cmd_words", "gpu_pixels"]

    def section(title, first_a, cnt_a, first_b, cnt_b):
        print(f"\n== {title}: A frames [{first_a},{first_a + cnt_a}) B frames [{first_b},{first_b + cnt_b}) ==")
        print(f"  {'metric':<20} {'stat':<5} {'A':>15} {'B':>15} {'B-A':>14} {'rel':>9}")
        for m in metrics:
            va = [r[m] for r in ra[first_a:first_a + cnt_a]]
            vb = [r[m] for r in rb[first_b:first_b + cnt_b]]
            print(f"  {m:<20} {'total':<5} " + fmt_delta(sum(va), sum(vb)))
            for p, name in ((0.5, "p50"), (0.9, "p90"), (0.99, "p99")):
                print(f"  {'':<20} {name:<5} " + fmt_delta(pct(va, p), pct(vb, p)))
            print(f"  {'':<20} {'max':<5} " + fmt_delta(max(va, default=0), max(vb, default=0)))

    section("all frames", 0, len(ra), 0, len(rb))
    wa, wb = sa["per_frame_timedemo_window"], sb["per_frame_timedemo_window"]
    kind = (sa.get("window") or {}).get("kind", "timedemo")
    if wa["frames"] and wb["frames"]:
        section(f"measurement window ({kind})", wa["first_frame"], wa["frames"], wb["first_frame"], wb["frames"])
    noise = load_noise(args.noise)
    wl = workload_of(sa) if workload_of(sa) == workload_of(sb) else None
    if have_model:
        ta, tb = sa["totals"], sb["totals"]
        print("\n== whole-run totals (including boot) ==")
        for m in ("insns", "est_cycles", "icache_miss_sdram", "dcache_miss", "mispredict", "softfloat_calls"):
            print(f"  {m:<20}       " + fmt_delta(ta[m], tb[m]))
        if "model_penalties" in sa and "model_penalties" in sb:
            print("  model penalty cycles:")
            for k in sa["model_penalties"]:
                print(f"    {k:<18}       " + fmt_delta(sa["model_penalties"][k], sb["model_penalties"].get(k, 0)))
        print("\n== layout noise floor (est_cycles; noise/noise_floor.json) ==")
        if noise is None:
            print(f"  not available ({args.noise} missing)")
        elif not wl or wl not in noise.get("workloads", {}):
            print(f"  no noise data for workload {wl!r} (have: {', '.join(sorted(noise.get('workloads', {})))})")
        else:
            for key, (xa, xb) in (("est_cycles", (ta["est_cycles"], tb["est_cycles"])),
                                  ("window_est_cycles", (wa.get("est_cycles", {}).get("sum", 0),
                                                         wb.get("est_cycles", {}).get("sum", 0))),
                                  ("dcache_miss", (ta["dcache_miss"], tb["dcache_miss"])),
                                  ("icache_miss_sdram", (ta["icache_miss_sdram"], tb["icache_miss_sdram"])),
                                  ("mispredict", (ta["mispredict"], tb["mispredict"]))):
                ln = noise_line(noise, wl, key, xa, xb)
                if ln:
                    print(ln)
            print("  A delta inside the band is indistinguishable from moving code/data around; it is not evidence "
                  "of a speedup.")

    if pa is not None and pb is not None:
        names = set(pa) | set(pb)
        zero = {"self_insns": 0, "self_cycles": 0, "icache_miss": 0, "dcache_miss": 0, "mispredict": 0,
                "calls": 0, "tailcalls": 0}
        rows = []
        for nm in names:
            x, y = pa.get(nm, zero), pb.get(nm, zero)
            rows.append((y["self_cycles"] - x["self_cycles"], y["self_insns"] - x["self_insns"], nm, x, y))
        tot_a = sum(r["self_cycles"] for r in pa.values()) or 1
        rows.sort(key=lambda r: (-abs(r[0]), r[2]))
        print(f"\n== top {args.top} per-function deltas by |self est_cycles| (whole run) ==")
        print(f"  {'function':<36} {'cyc A':>13} {'cyc B':>13} {'dcyc':>12} {'%tot':>7} {'dinsns':>12} "
              f"{'dcalls':>9} {'dic':>7} {'ddc':>7} {'dbp':>8}")
        for dc, di, nm, x, y in rows[:args.top]:
            if dc == 0 and di == 0:
                break
            tag = "" if nm in pa and nm in pb else (" [B only]" if nm in pb else " [A only]")
            print(f"  {(nm + tag)[:36]:<36} {x['self_cycles']:>13,} {y['self_cycles']:>13,} {dc:>+12,} "
                  f"{100.0 * dc / tot_a:>+7.3f} {di:>+12,} {y['calls'] + y['tailcalls'] - x['calls'] - x['tailcalls']:>+9,} "
                  f"{y['icache_miss'] - x['icache_miss']:>+7,} {y['dcache_miss'] - x['dcache_miss']:>+7,} "
                  f"{y['mispredict'] - x['mispredict']:>+8,}")

    ok = not fails
    print("\nRESULT: " + ("EQUIVALENT (hash_px, hash_z, hash_pal identical on every aligned frame, same frames, "
                          "same workload" + (", no new memory-coherency hazards)" if ha is not None and hb is not None
                                             else ")")
                          if ok else "NOT EQUIVALENT: " + "; ".join(fails)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
