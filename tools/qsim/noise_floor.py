#!/usr/bin/env python3
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

"""noise_floor.py --baseline ELF [--app quake] [--elfs DIR] -- layout noise floor for est_cycles.

Runs the baseline ELF and every functionally identical layout variant of it (apps/<app>/noise/elfs/
*.elf; apps/quake/noise/build_variant.sh is the worked example of how to build them) over each
workload with --profile, eager/flat memory and caps.heap_size pinned to the baseline's value, and
writes apps/<app>/noise/noise_floor.json:

  workloads.<workload>.<metric> = {baseline, band_pct, stdev_pct, variants, deltas_pct{variant: %}}

compare.py prints the band next to an A/B est_cycles delta: a delta inside the band is what moving
code and data around costs, not a speedup.  The variants also have to render identically - the
script checks the frame hashes and reports any variant that does not (that would be a qsim or a
build problem, not noise).
"""
import argparse
import csv
import json
import os
import statistics
import subprocess
import sys

QSIM_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, QSIM_DIR)
METRICS = ["est_cycles", "window_est_cycles", "insns", "dcache_miss", "icache_miss_sdram", "mispredict"]


def app_dir(name):
    d = name if os.path.isdir(name) else os.path.join(QSIM_DIR, "apps", name)
    if not os.path.isdir(d):
        sys.exit(f"noise_floor.py: no such app: {name}")
    return os.path.abspath(d)


def app_profile(d):
    p = [f for f in sorted(os.listdir(d)) if f.endswith(".app")]
    if len(p) != 1:
        sys.exit(f"noise_floor.py: {d} must contain exactly one *.app profile")
    return os.path.join(d, p[0])


def workload_sh(d, fn):
    return subprocess.run(["bash", "-c", f'source "{d}/workloads/workloads.sh"; {fn}'],
                          capture_output=True, text=True, check=True).stdout


def run(elf, wl, out, adir, data_dir, heap):
    opts = [o for o in workload_sh(adir, f"workload_opts {wl}").split("\n") if o != ""]
    cmd = [os.path.join(QSIM_DIR, "qsim"), elf, "--app", app_profile(adir), "--out", out, "--quiet",
           "--profile", "--heap-size", heap, "--gpu-sched", "eager", "--mem", "flat"] + opts
    if data_dir:
        cmd[3:3] = ["--data-dir", data_dir]
    return cmd


def metrics_of(d):
    s = json.load(open(os.path.join(d, "summary.json")))
    t = s["totals"]
    w = s["per_frame_timedemo_window"]
    m = {"est_cycles": t["est_cycles"], "insns": t["insns"], "dcache_miss": t["dcache_miss"],
         "icache_miss_sdram": t["icache_miss_sdram"], "mispredict": t["mispredict"],
         "window_est_cycles": w.get("est_cycles", {}).get("sum", 0)}
    rows = list(csv.DictReader(open(os.path.join(d, "frames.csv"))))
    oracle = (s["oracle"]["hash_px_all"], s["oracle"]["hash_z_all"], s["oracle"]["hash_pal_all"], len(rows),
              s["status"], s["timedemo_result"])
    return m, oracle


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app", default="quake", help="app name under apps/, or an app directory")
    ap.add_argument("--baseline", required=True, help="the reference ELF the variants were built from")
    ap.add_argument("--elfs", default=None, help="directory of layout-variant ELFs (default apps/<app>/noise/elfs)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--json", default=None)
    ap.add_argument("--workloads", default=None)
    ap.add_argument("--data-dir", default=os.environ.get("QSIM_DATA_DIR", os.environ.get("QSIM_PAK_DIR", "")))
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    a = ap.parse_args()

    adir = app_dir(a.app)
    ndir = os.path.join(adir, "noise")
    a.elfs = a.elfs or os.path.join(ndir, "elfs")
    a.out = a.out or os.path.join(ndir, "runs")
    a.json = a.json or os.path.join(ndir, "noise_floor.json")

    wls = (a.workloads or workload_sh(adir, "workload_default_set")).split()
    heap = subprocess.run(["bash", "-c", f'''python3 - "{a.baseline}" <<'EOF'
import sys
from elftools.elf.elffile import ELFFile
e = ELFFile(open(sys.argv[1], "rb"))
top = max(s["p_vaddr"] + s["p_memsz"] for s in e.iter_segments()
          if s["p_type"] == "PT_LOAD" and s["p_memsz"] and s["p_vaddr"] >= 0x10400000)
print(hex(0x13A00000 - ((top + 15) & ~15)))
EOF'''], capture_output=True, text=True, check=True).stdout.strip()

    elfs = {"baseline": os.path.abspath(a.baseline)}
    for f in sorted(os.listdir(a.elfs)):
        if f.endswith(".elf") and f != "baseline.elf":
            elfs[f[:-4]] = os.path.join(a.elfs, f)
    # the -g build is the same code as the baseline (coverage mapping), not a layout variant
    elfs.pop("g", None)
    print(f"noise floor: {len(elfs)} ELFs x {len(wls)} workloads, heap {heap}")

    os.makedirs(a.out, exist_ok=True)
    jobs = []
    for name, elf in elfs.items():
        for wl in wls:
            out = os.path.join(a.out, f"{name}.{wl}")
            if os.path.exists(os.path.join(out, "summary.json")):
                continue
            jobs.append((name, wl, run(elf, wl, out, adir, a.data_dir, heap)))
    if jobs:
        print(f"running {len(jobs)} profile runs, {a.jobs} parallel")
        procs = []
        import time
        for name, wl, cmd in jobs:
            while len([p for p in procs if p.poll() is None]) >= a.jobs:
                time.sleep(0.2)
            procs.append(subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        rc = [p.wait() for p in procs]
        if any(rc):
            print(f"WARNING: {sum(1 for r in rc if r)} of {len(rc)} runs exited nonzero")

    data = {}
    oracles = {}
    for name in elfs:
        for wl in wls:
            out = os.path.join(a.out, f"{name}.{wl}")
            m, o = metrics_of(out)
            data[(name, wl)] = m
            oracles[(name, wl)] = o
    bad = [f"{name}.{wl}" for (name, wl), o in oracles.items() if o != oracles[("baseline", wl)]]
    if bad:
        print(f"WARNING: these variants do not render identically to the baseline: {', '.join(sorted(bad))}")

    js = {"generated_by": "tools/qsim/noise_floor.py", "app": os.path.basename(adir), "heap_size": heap, "elfs": {}, "workloads": {}}
    for name, elf in elfs.items():
        js["elfs"][name] = os.path.relpath(elf, QSIM_DIR)
    for wl in wls:
        js["workloads"][wl] = {}
        for metric in METRICS:
            base = data[("baseline", wl)][metric]
            deltas = {n: (100.0 * (data[(n, wl)][metric] - base) / base if base else 0.0)
                      for n in elfs if n != "baseline"}
            # address-only variants (link-script padding) execute the SAME instructions; the
            # -falign-* variants also change how much alignment padding is executed
            addr = {n: v for n, v in deltas.items()
                    if data[(n, wl)]["insns"] == data[("baseline", wl)]["insns"]}
            js["workloads"][wl][metric] = {
                "baseline": base,
                "band_pct": max(abs(v) for v in addr.values()) if addr else 0.0,
                "band_all_pct": max(abs(v) for v in deltas.values()) if deltas else 0.0,
                "stdev_pct": statistics.pstdev(list(addr.values())) if len(addr) > 1 else 0.0,
                "variants": len(addr),
                "variants_all": len(deltas),
                "address_only_variants": sorted(addr),
                "deltas_pct": {k: round(v, 4) for k, v in sorted(deltas.items())},
                "renders_identically": not [b for b in bad if b.endswith("." + wl)],
            }
    json.dump(js, open(a.json, "w"), indent=1, sort_keys=True)
    print(f"wrote {a.json}")
    print(f"{'workload':<10} {'metric':<20} {'baseline':>16} {'band %':>9} {'sd %':>8} {'band all %':>11}  "
          "worst variant (all)")
    for wl in wls:
        for metric in METRICS:
            e = js["workloads"][wl][metric]
            worst = max(e["deltas_pct"], key=lambda k: abs(e["deltas_pct"][k])) if e["deltas_pct"] else "-"
            print(f"{wl:<10} {metric:<20} {e['baseline']:>16,} {e['band_pct']:>9.3f} {e['stdev_pct']:>8.3f} "
                  f"{e['band_all_pct']:>11.3f}  {worst} {e['deltas_pct'].get(worst, 0):+.3f}%")
    print("band % = max |delta| over the address-only variants (same instructions, different addresses); "
          "band all % also includes the -falign-functions/-falign-loops builds, which execute different "
          "amounts of alignment padding.")


if __name__ == "__main__":
    main()
