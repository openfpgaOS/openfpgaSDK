# qsim — openfpgaOS app simulator, profiler and rendering-equivalence gate

`qsim` boots a **production openfpgaOS app ELF** — the same file you would
copy to an SD card — inside an emulated os25 Analogue Pocket, and produces
two things:

1. a **per-function profile** (cycles, cache misses, mispredicts, soft-float
   and service calls, caller attribution), and
2. a **per-frame pixel/z/palette hash** used as a bit-exact
   rendering-equivalence oracle.

It is a native x86 program. No FPGA, no SD card, no device. A full Quake
timedemo — 1.86 G guest instructions, 62 M rasterized pixels — runs in about
4.5 s without profiling and 10.5 s with it (400+ and ~175 guest MIPS on one
core).

Nothing in the simulator is app-specific. The one file that knows about your
app is a plain-text **app profile** (`apps/<app>/<app>.app`), and the worked
example shipped here is the Quake port (`apps/quake/`).

```
tools/qsim/
├── README.md              this file
├── ENV_SPEC.md            the emulated environment: loader, ecalls, services, caps, MMIO, files
├── Makefile               make -> ./qsim        (cpu-test, gputest: Makefile.cpu/.gpu)
├── run.sh                 one run of one workload
├── gate.sh                THE equivalence gate: every workload x every mode, one PASS/FAIL line
├── compare.py             A/B comparison and gate rules
├── coverage.py            which source functions a set of runs actually executed
├── noise_floor.py         how much of an est_cycles delta is code placement
├── qsim_app.sh            app selection shared by run.sh and gate.sh
├── addr2sym.py segcmp.py  small helpers
├── cpu.[ch] cpu_exec.h cache.[ch] fpu.[ch]    VexiiRiscv rv32imafc core + os25 timing model
├── gpu.[ch] gputest.c gpuvec.h                os25 gpu_core raster model (validated against RTL)
├── lint.[ch]                                  D-cache/GPU memory-coherency model
├── env.[ch] qelf.[ch] prof.[ch] main.c        kernel environment, ELF loader, profiler, driver
├── app.[ch]                                   the app profile: the only app-aware code
├── tests/cpu/                                 CPU self-test suite
└── apps/
    └── quake/             the worked example: profile, .ini, workloads, measurements
```

---

## When to use it

**Optimizing or refactoring an app without a device.** The profile is
per-function and per-frame, with I-cache/D-cache/mispredict/soft-float
attribution and a caller table, so "where does the frame go" has an answer
before you touch the hardware.

**Proving a change renders identically.** `gate.sh <before.elf> <after.elf>`
runs both ELFs over every workload in five GPU-schedule and memory modes and
fails if a single pixel, z value or palette entry differs on any frame — or if
the candidate introduces a GPU-synchronization or D-cache-coherency hazard the
reference does not have. That is the difference between "it still looks right
on my Pocket" and "it is the same program".

**Checking what a test actually covers.** `coverage.py` maps executed PCs
back to source functions through DWARF, inlined copies included, and lists the
render functions no workload ever reached.

**What it is not.** It is not a cycle-accurate model of the device and it is
not a substitute for running on hardware. `est_cycles` is a relative A/B
metric (see *Limits*), kernel time is charged 0, and SDRAM contention from the
GPU, audio and scanout is not modelled.

---

## Quick start

```sh
make                                          # builds ./qsim, gcc -O2 -march=native

export QSIM_DATA_DIR=/path/to/quake/id1       # the app's data files (PAK0.PAK, PAK1.PAK)

./run.sh app.elf out/demo1 demo1              # one workload, with --profile
./gate.sh before.elf after.elf out/gate       # the equivalence gate: PASS or FAIL
./compare.py out/A out/B                      # one run pair, full report
```

`run.sh` writes `out/demo1/{frames.csv,summary.json,profile.txt,console.log}`
and screenshots; `gate.sh` writes `out/gate/{A,B}/<workload>.<mode>/` plus
`out/gate/gate.txt` and prints one final `GATE: PASS` / `GATE: FAIL (...)`
line. Exit status: 0 PASS, 1 FAIL, 2 setup error.

Pick the app with `QSIM_APP` (default `quake`); it names a directory under
`apps/`, or any directory laid out the same way.

`run.sh` environment overrides:

| variable | effect |
|---|---|
| `QSIM_APP=<name\|dir>` | app under test (default `quake`) |
| `QSIM_DATA_DIR=<dir>` | host directory with the app's data files (`QSIM_PAK_DIR` is the older name) |
| `QSIM_FAST=1` | no profiling, about 2.5x faster; the model columns are 0 |
| `QSIM_PPM_EVERY=K` | screenshot every K frames; 0 = none |
| `QSIM_GPU_SCHED=eager\|lazy\|drain` | `--gpu-sched` (default eager) |
| `QSIM_MEM=flat\|lint\|strict` | `--mem` (default flat) |
| `QSIM_SEED_MEM=S` | `--seed-mem` (default 0 = uninitialized memory reads as zero) |
| `QSIM_HEAP_SIZE=N` | `--heap-size` (default: device-exact, follows the ELF's `.bss`) |
| `QSIM_EXTRA="--frames 300"` | extra qsim options |

A single `run.sh` + `compare.py` pair checks one workload in one GPU schedule
and one memory mode. It cannot see missing GPU sync barriers or D-cache
coherency bugs, and one demo playback exercises only the code paths that demo
happens to take. **Use `gate.sh` to accept a patch.**

---

## Onboarding a new app

An app needs three things, none of which are qsim's business:

1. **an ELF** — the production build, unmodified;
2. **its data files** — whatever the kernel would serve from SD card slots;
3. **a deterministic workload** — a fixed input sequence that ends on a
   console line. A recorded demo, a benchmark mode, or a scripted session
   fed in through a config file. If two runs of the same ELF do not produce
   identical output, there is nothing to compare.

Create `apps/<app>/` with an app profile and a workload table:

```
apps/<app>/
├── <app>.app                    the app profile (below)
├── <app>.ini.in                 the .ini the kernel serves, with $ARGS
└── workloads/workloads.sh       workload_names / workload_default_set / workload_opts / workload_inputs_sha
```

### The app profile

`key = value` lines, `#` comments, double quotes to protect spaces,
`slot` and `parity` repeatable. Everything is optional except `elf_name`.
Relative paths resolve next to the profile. This is the whole format:

| key | meaning |
|---|---|
| `name` | recorded in `summary.json` |
| `elf_name` | the guest `argv[0]` (the kernel builds it from the `.ini` `ELF=` key) |
| `default_args` | guest argv after argv[0] when `--args` is not given |
| `build_stamp` | prefix of a `"<prefix> HH:MM:SS Mon DD YYYY"` build stamp in the image; qsim rewrites the date so two builds of the same source made at different times render identical console text |
| `frame_symbol` | ELF symbol holding the app's host frame counter (used to align A/B runs) |
| `zbuffer_symbol` | ELF symbol holding the depth buffer (so `hash_z` can be taken before the frame's z clear) |
| `result_line` | POSIX ERE: the console line that ends the run; capture group 1 is the number of host frames in the measurement window |
| `error_line` | POSIX ERE: the console line that means the app failed |
| `parity <name> <ERE>` | a fact that must match for two runs to be the same workload (Quake: the heap and surface-cache sizes the engine prints); recorded in `summary.json` and compared by the gate |
| `slot <id> <name> <kind>` | one data-slot registry entry, in probe order |

Slot kinds: `stub:<bytes>` (a registry entry of that size; reads return
zeros — this is how `os.bin` and the app ELF itself appear), `config:<file>`
(the app's `.ini`, with `$ARGS` expanded; also answers `config_get`),
`data:<file>` (a read-only host file under `--data-dir`, matched
case-insensitively), and `nv` (a writable nonvolatile slot that starts empty).

A minimal profile for an app with one data file:

```
name         = MyGame
elf_name     = mygame.elf
default_args = +bench 1
result_line  = ^BENCH DONE ([0-9]+) frames
slot = 1  os.bin      stub:135400
slot = 2  mygame.ini  config:mygame.ini.in
slot = 3  mygame.elf  stub:876096
slot = 4  game.grp    data:game.grp
slot = 8  mygame.cfg  nv
```

With no `--app` at all, qsim uses a built-in profile: three stub slots, no
data files, no console conventions. That is enough to boot an ELF and count
instructions; it is not enough to gate one.

### The workload table

`workloads/workloads.sh` is sourced by `run.sh` and `gate.sh`, so both always
agree on what a workload is. It defines four shell functions:

```sh
workload_names()        { echo "bench1 bench2 play1"; }
workload_default_set()  { echo "bench1 bench2 play1"; }   # what gate.sh runs
workload_opts()         { ... }   # the qsim options of one workload, one per line
workload_inputs_sha()   { ... }   # sha256 of files the workload reads (gate.sh run-reuse stamps)
```

`workload_opts` prints `--label <name>` plus whatever the workload needs:
`--args`, `--file NAME=PATH` to serve a host file as an extra data slot (ids
20+ — this is how a scripted session gets its console script), and
`--window-on` / `--window-end-on` / `--end-on` console markers for workloads
that have no natural result line. See `apps/quake/workloads/workloads.sh`.

### Aim for coverage, not for one demo

A demo playback runs the renderer and nothing else. The Quake example learned
this the hard way: adding one live-game workload (server, script VM, physics,
loopback client) added 655 M instructions per run — 23% of it — that no demo
executes, and adding one workload that turns the default-off render paths on
immediately caught a real pixel change and a real coherency bug that three
demos had passed. Write at least one workload that is not a recording.

---

## The gate

```sh
./gate.sh <elfA> <elfB> <outdir>
```

Every workload x five modes x two ELFs, in parallel (`xargs -P $(nproc)`), in
fast mode. For the five Quake workloads that is 50 runs in about 30 s on 16
cores. A run directory whose `gate.stamp` matches (ELF sha256, qsim sha256,
app profile, workload definition and inputs, options) is **reused**, so
copying `A/` from an earlier gate makes each further candidate cost half.

| mode | qsim options | catches |
|---|---|---|
| `eager-lint` | `--gpu-sched eager --mem lint` | rendering changes, plus hazard counts |
| `lazy-lint` | `--gpu-sched lazy --mem lint` | missing or weakened CPU/GPU sync barriers |
| `eager-strict` | `--gpu-sched eager --mem strict` | D-cache coherency bugs, as pixel differences |
| `lazy-strict` | `--gpu-sched lazy --mem strict` | both at once |
| `eager-seed` | `--gpu-sched eager --mem lint --seed-mem 0x5eed…` | reads of never-initialized memory |

Also available on request: `drain-lint`, `eager-flat`.

**It FAILS when, in any run pair:**

1. `hash_px`, `hash_z` or `hash_pal` differ on any aligned frame;
2. a frame exists in only one run (`--allow-boot-flips` permits this before
   the first host frame);
3. a run did not finish the same way (status differs, or both errored), or a
   run is missing;
4. the workload parity differs — `caps.heap_size`, or one of the app
   profile's `parity` facts;
5. B introduces a memory-coherency **hazard class** (with the functions
   listed) that A does not have in that run.

**Frames are aligned by content, not by row.** The measurement window is
matched by position; every other frame by `(host_framecount, n-th flip in that
host frame)`, and when one host frame has a different number of flips in the
two runs its flips are matched by content. A console or loading flip that
appears or disappears stays a local difference instead of shifting every later
frame.

**Reference races are excluded, not ignored.** In every mode except
`eager-lint`, a frame that A itself renders differently than in its own
`eager-lint` run is a race *in the reference*: under that mode its content is
layout-dependent garbage, not an oracle. Such frames are dropped from that
mode's comparison and listed as `reference-racy`; `eager-lint` still compares
them, and B's own schedule-dependent frames are never excluded. In a strict
mode a frame where either side hit a coherency hazard is excluded for the same
reason.

### Why lazy-GPU and strict-memory are what make it trustworthy

This is the single most important thing to understand about the gate.

An earlier version ran only `eager` (infinitely fast GPU) and `flat` memory.
It **passed all three of these one-instruction sabotage ELFs**, each of which
would render wrongly or racily on a real Pocket:

| sabotage | eager + flat | the mode that catches it |
|---|---|---|
| skip the `GPU_FENCE_REACHED` wait in `prepare_framebuffer_for_cpu` | identical on all 1000 frames | `lazy`: `hash_px` differs on 610 frames, `hash_z` on 540 |
| make `cache_clean` a no-op (no writeback before GPU texture reads) | identical; insns −0.28% | `lint`: 54,554 GPU reads of uncommitted CPU stores (reference: 0) |
| move the z-buffer to the **cached** alias | identical; **est_cycles −0.9%, i.e. reported as a win** | `lint`/`strict`: 86,240 hazards; `strict`: 264 frames differ |

An infinitely fast GPU never lets a missing barrier matter. Flat memory means
the CPU and the GPU share one array, so a missing cache clean is invisible.
Both assumptions are comfortable and both are wrong. `lazy` runs each command
as late as the hardware legally could; `strict` splits the L1 D-cache view
from committed SDRAM so a coherency bug changes pixels. **Do not accept a
patch on `eager-flat` alone.**

### GPU schedules (`--gpu-sched`)

Published command words go into a queue standing for the ring BRAM between
`ring_rdptr` and `ring_wrptr`. The CPU can observe GPU progress in exactly
three ways, and the schedule decides how far the GPU has got at each:

| CPU observation | eager (default) | lazy (as late as legal) | drain |
|---|---|---|---|
| publish (CTRL bit 3) | runs everything | nothing | nothing |
| `GPU_FENCE_REACHED` read | — | runs up to and including the next queued FENCE/FLIP | runs everything |
| `GPU_RING_RDPTR` read | — | runs **one** command | runs everything |
| kernel `video_acquire_next(idx, token)` fence wait | — | runs until `fence_reached >= token` | runs everything |

Not observations: `GPU_STATUS` (busy/ring_empty follow the queue with no side
effect), `video_flip` / `video_wait_flip` (vblank, not GPU progress), the
cache services. Register writes take effect when written, as in the RTL
(`GPU_TEX_FLUSH`, `GPU_PALOOKUP_BASE`, the translucency LUT). At end of run
the queue is drained, so every schedule reports the same frames.

Not modelled: `acquire_next`'s ~5 ms spin limit (the kernel wait is treated as
unbounded), RDPTR advancing inside a command, and any real GPU pixel rate —
lazy is a legal extreme, not a timing model.

### Memory coherency (`--mem lint|strict`)

The model keeps its own os25 L1 D-cache (1024 sets x 2 ways x 64 B, LRU),
driven by every CPU cached load/store, every `cbo`, the kernel cache services
and every store the emulated kernel itself performs. Semantics come from the
VexiiRiscv source (`LsuPlugin.scala:221-238`, `LsuL1Plugin.scala:886-966`) and
`hal/cache.c`:

| event | model |
|---|---|
| CPU cached store | line loaded (miss evicts the LRU way, written back if dirty) and dirty |
| CPU cached load | line loaded; returns the L1 copy |
| `cbo.clean` / `cbo.flush` (FLUSH decodes as CLEAN) | dirty line written back, **stays loaded** |
| `cbo.inval` (M-mode) | line dropped **without** writeback |
| `cache_clean/flush/inval_range` | per-line cbo over `[a & ~63, a + n)`, clamped to SDRAM |
| `cache_flush` | `dcache_evict_all`: 2048 conflict reads at the top of SDRAM |
| GPU / uncached-alias write | goes to SDRAM; a loaded L1 line becomes **stale** |
| GPU read, uncached read, scanout | read SDRAM |

`--mem lint` leaves memory flat and only counts. `--mem strict` splits it: the
GPU, the uncached alias, frame hashes, z snapshots and screenshots use a
committed image C, while the CPU's cached view V diverges until a device event
moves data (writeback V→C; inval or eviction of a stale line C→V). The
invariant "V == C on every line that is neither dirty nor stale" is checkable
with `--check-strict-invariant`.

Hazard classes (their sum is the `mem_hazards` column):

| class | meaning |
|---|---|
| `gpu_read_of_uncommitted_cpu_store` | GPU read of a byte still dirty in L1 — a missing `of_cache_flush_range` |
| `cpu_cached_read_of_stale_line` | CPU cached load of bytes the GPU changed while the line was loaded — a missing invalidate |
| `writeback_reverts_gpu_or_uncached_write` | a dirty writeback landing on top of GPU pixels |
| `cpu_uncached_read_of_uncommitted_store` | uncached-alias load of bytes still dirty in L1 |
| `cbo_inval_dropped_dirty_line` | inval discarded CPU stores |

Two worst-case classes that ignore eviction (`wc_*`) are advisory only.
`summary.json` records `memory_coherency.sites` — `(class, function, pc,
count)` for every hazard — and `addr2sym.py <elf> <addr>…` names a pc.

**Counts are layout noise; classes and sites are not.** The same bug in two
builds of the same source that differ only in `-falign-functions` counted
180,943 and 267,844 hazards. That is why rule 5 is about classes.

Not modelled: the kernel's own cached traffic (it can only commit lines
*earlier*), store-buffer and writeback-FIFO latency (a writeback commits at
the cbo instant — hardware has been seen reading stale lines after a
clean-only), and SDRAM arbiter priority.

---

## Limits — read these before quoting a number

* **`est_cycles` is a relative metric with a ±0.6% noise floor.** Seven
  functionally identical builds of the same source (link padding,
  `-falign-functions`, `-falign-loops`) spread `est_cycles` by 0.56–0.64% per
  workload while executing *exactly the same instructions*. D-cache misses
  move by up to 3% and I-cache misses by up to 35% for the same reason. **A
  win below about 0.6% is code placement, not a speedup.** `noise_floor.py`
  measures the band for your app and `compare.py` prints it next to the delta.
* **Kernel-side work costs zero instructions and zero cycles.** Services,
  ecalls, the file cache and DMA are free in the model. Their call counts are
  in `profile.txt`, but their cost is not. In Quake's case the audio drain
  alone is an estimated 2–4% of device CPU cycles that the profile does not
  show.
* **Several timing parameters are GUESS values**, not derived from the RTL:
  the uncached-alias and MMIO access costs, BRAM latencies, most FPU result
  latencies (`lat_fadd`, `lat_fmul`, `lat_fdiv`, `lat_i2f`,
  `lat_fload`), the SDRAM write-burst setup, the I-cache REDO cost and the
  misaligned-trap cost. Every parameter carries its provenance in
  `qsim_model_default_params()` (cache.c) and in *The timing model* below.
* **`hash_cmd` embeds addresses**, so it is meaningful only between runs of
  the *same* ELF. Across ELFs, only `hash_px`, `hash_z` and `hash_pal`
  compare — which is exactly what the gate uses.
* **`caps.heap_size` follows `.bss`** on the device (`0x13A00000 - bss_end`),
  so a candidate with more static data would run a *different* workload.
  `compare.py` fails on that; `gate.sh` pins `--heap-size` for both ELFs.
* **The GPU is infinitely fast by default** and `est_cycles` never includes
  time spent waiting for the GPU or for vblank (`gpu_wait_px` shows the queued
  work at each wait).
* **Buffer rotation is instant-vblank** (1,0,1,0); the device typically uses
  three buffers.
* **FPU subnormals**: qsim is exact IEEE-754; the device FPU is generated with
  `--fpu-ignore-subnormal`. `summary.json` reports subnormal operand/result
  counts so you know whether it can matter (Quake demo1: 0 of 62 M F ops).
* **Seeded memory** catches reads of memory nobody ever wrote. It cannot catch
  reads of stack or heap memory that earlier code did write; that needs a
  Valgrind-style shadow.
* **Coverage** is per source function, not per branch, and is measured on
  whichever ELF you ran.
* **The noise floor is a property of the model, not of the device.** It says
  how much of a reported change is placement. It does not say the device would
  show the same change.

---

## Command-line reference

```
qsim <elf> --out <dir> [--app <profile>] [--data-dir <dir>] [options]
  --app FILE               app profile (apps/<app>/<app>.app); default: a bare app, no data files
  --data-dir DIR           host directory holding the app's data files (the profile's data: slots)
  --args "<argv>"          guest arguments after argv[0]; default: the profile's default_args
  --frames N               stop after N presented frames (default: stop at the profile's result line)
  --profile                profiling interpreter + os25 timing model (est_cycles, cache, branch
                           columns; profile.txt/.tsv)
  --ppm-every K [--ppm-start S]   out/ppm/frame_NNNNN.ppm with the palette in effect at the flip
  --gpuvec F1,F2,...       capture these frames' complete GPU input as out/gpuvec/frame_N.vec
  --no-gpu-model           do not rasterize: flip/fence only (a different run, not a faster one)
  --gpu-sched eager|lazy|drain    GPU schedule (default eager)
  --mem flat|lint|strict   memory model (default flat)
  --check-strict-invariant (debug, slow) verify the split-memory invariant at every flip
  --no-normalize-build-stamp   keep the ELF's build stamp as built
  --heap-size N            override caps.heap_size (default: device-exact, 0x13A00000 - bss_end)
  --file NAME=PATH         serve a host file as read-only data slot NAME (ids 20+, repeatable):
                           apps open data files by basename, so this is how a console script arrives
  --end-on TEXT            stop (status workload_complete) when a guest console line contains TEXT
  --window-on TEXT [--window-end-on TEXT]   measurement window = frames published from the host frame
                           that prints TEXT (to the one that prints the end marker)
  --seed-mem SEED          fill memory the device does not initialize (app stack, unallocated
                           SDRAM/BRAM, CRAM0) with a deterministic PRNG pattern instead of zeros
  --coverage               (implies --profile) write coverage.tsv: every executed pc with its count
  --label NAME             workload name recorded in summary.json; run.sh/gate.sh set it
  --max-insns N, --top N, --echo, --quiet, --verbose
```

Exit status is 0 when the run completes or hits a limit, and 1 on a guest
error: a trap (ebreak, illegal instruction, access fault — reported with the
function and `ra`), the app profile's `error_line`, an unhandled ecall or
service, or a watchdog (no frame for 20e9 instructions).

### Outputs

| file | content |
|---|---|
| `frames.csv` | one row per presented frame (below) |
| `summary.json` | app, workload, status, result line, parity facts, ELF sha256 and path, argv, heap size, seed, window, extra files, wall time, MIPS, virtual clock, env counters, whole-run totals, boot cost, per-frame mean/p50/p90/p99/max/min/sum of every metric for all frames and for the measurement window, chained oracle hashes, GPU sync statistics, memory-coherency classes and sites, FPU subnormal counts, model penalty breakdown |
| `profile.txt` | `--profile`: functions by self est_cycles with self insns, % of insns and cycles, cycles/insn, I-cache and D-cache misses, mispredicts, calls, tail calls; soft-float and libm tables **by caller**; OS service calls by caller; ecall counts; GPU statistics |
| `profile.tsv`, `profile_calls.tsv` | the same rows and caller→callee edges, machine-readable |
| `console.log` | everything the guest wrote to stdout/stderr |
| `ppm/frame_NNNNN.ppm` | screenshots of the displayed buffer |
| `gpuvec/frame_N.vec` | `--gpuvec` captures (replay with `gpuvec_check.sh`) |
| `coverage.tsv` | `--coverage`: every executed pc and its count (input to `coverage.py`) |

`frames.csv` columns:

| column | meaning |
|---|---|
| `frame` | flip sequence number: the n-th GPU `CMD_FLIP` executed |
| `host_framecount` | the app's frame counter (`frame_symbol`) at the flip — the A/B alignment key |
| `buffer` | displayed buffer index |
| `insns` | guest instructions since the previous flip (service hooks excluded; frame 0 includes boot) |
| `est_cycles` | timing-model cycles since the previous flip (`--profile`, else 0) |
| `icache_miss_sdram`, `icache_miss_bram`, `dcache_miss`, `mispredict` | model events in the frame |
| `softfloat_calls` | calls and tail calls landing on libgcc soft-float entry points |
| `gpu_cmd_words`, `gpu_pixels` | command words executed and source pixels generated |
| `hash_cmd` | FNV-1a-64 of those raw command words, **addresses included** — same-ELF only |
| `hash_px` | **the rendering oracle**: FNV-1a-64 of the displayed framebuffer at the flip |
| `hash_z` | FNV-1a-64 of the frame's final z-buffer, taken just before the z clear (0 when the frame did not clear z) |
| `hash_pal` | FNV-1a-64 of the 256-entry palette |
| `mem_hazards` | cache-aware coherency hazards in the frame (`--mem lint\|strict`) |
| `gpu_barriers` | CPU observations of GPU progress that had work behind them |
| `gpu_wait_px` | GPU pixels executed at those observations: the work the CPU waited for |

*Why `hash_z` is captured before the clear:* renderers typically queue the
next frame's z clear ahead of the flip, so at the flip the z-buffer is already
zero. qsim sees every command before it executes and hashes the z-buffer
(identified by `zbuffer_symbol`, or by a 2·width-byte rect with 2·width
stride) right before the clear, reporting it with that frame's flip.

---

## The emulated environment

Full specification in **ENV_SPEC.md**. In short:

* **Loader**: absolute PT_LOADs (BRAM 0x4000–0x7800 or SDRAM
  0x10400000–0x13A00000), memsz zero fill, `bss_end` aligned to 16, SysV stack
  at 0x13A80000 with auxv PAGESZ/HWCAP/RANDOM/OF_CAPS/OF_SVC.
* **Caps (os25)**: `hw_features 0x01A7E35B`, `heap_size = 0x13A00000 -
  bss_end`, CPU ring, `tex_fast_size 0`.
* **Services**: the 106-entry table, each entry a hook address that runs C code
  and returns to `ra` — video (ports of the kernel role model with instant
  vblank), audio (a 131072-pair ring drained at 48 kHz of virtual time), mixer
  (unique nonzero handles), input (zeroed structs), config (answers from the
  app's `.ini`). Anything unimplemented is fatal and names the caller.
* **ecalls**: Linux 25/29/35/56/57/61–66/93/94/96/214–216/222/226/291/414/422
  plus getpid/sigaction/madvise; `brk`/`mmap2`/`munmap` are verbatim ports of
  the kernel allocator; vendor TIMER, FILE and ANALOGIZER are handled.
* **Files**: the data-slot registry from the app profile, in probe order.
* **Virtual clock**: starts at 5 s; a timer read from an ecall PC that already
  read since the last advance adds a 200 000 µs quantum, which makes wall time
  advance by exactly one frame per host frame.
* **MMIO**: GPU at 0x4A000000; CRAM0 is RAM; sysreg 0x40000000 (`SYS_CYCLE` =
  `instret x 3` [CHOICE], deterministic and identical in fast and profile
  mode). Any other address raises a load/store access fault reported with the
  exact pc.
* **Memory**: all SDRAM and stack start zeroed unless `--seed-mem` is given.

---

## The GPU raster model

`gpu.c` implements the os25 `gpu_core` command set: span and perspective-span
groups, the parametric span list (attr modes, Z write/test, Q29 scale), clear
rect (plain and strided), texture/palookup/translucency uploads, fences and
flips. Semantics are per-pixel exact, in RTL order.

It is **validated byte-for-byte against the RTL**. `gpu_equiv.sh` and
`gpu_trace_equiv.sh` replay randomized command traffic through the Verilator
model of `gpu_core` and the C model and compare the displayed framebuffer, the
fence register and a hash of all 64 MB of SDRAM. `gpuvec_check.sh` does the
same with real frames captured from a running app (`--gpuvec`), and also
checks the replayed framebuffer hash against the `hash_px` qsim recorded —
which proves the capture is complete. For the Quake baseline, 55 distinct
demo1 frames replayed byte-identically.

---

## The timing model

`est_cycles = instructions + sum(penalties)`, charged on a running cycle clock
and attributed to the PC that incurred each one.

* **I-cache**: 256 sets x 2 ways x 64 B, LRU, next-line prefetcher, used for
  BRAM code too. A miss costs the REDO refetch plus the wait for the 64-bit
  bank word holding the instruction.
* **D-cache**: 1024 sets x 2 ways x 64 B, SDRAM only. `cbo.flush` decodes as
  CLEAN and leaves the line loaded. Load miss = REDO + refill wait (dirty
  victim written back first). Store miss goes to the 2-slot/16-op store
  buffer. Loads hitting a store-buffer line REDO until it drains. BRAM, CRAM0,
  MMIO and the 0x5xxxxxxx alias use the uncached io path.
* **Prediction**: 256-set direct-mapped BTB (16-bit hash), 4096-counter GShare
  with 12-bit history, 8-deep RAS updated only on BTB hits. Correctly
  predicted taken branch is charged 2 extra cycles; mispredict is charged 8.
  These additive penalties omit overlap between fetch and execution. The
  os25 RTL runs a warmed `addi; bnez` loop in about 2.08 cycles per iteration,
  including counter overhead, rather than the 4 cycles this model suggests.
  Treat prediction-parameter sweeps as sensitivity estimates, not measured
  CPU speedups; validate them on the RTL with application workloads.
* **Dependency stalls**: every register write records when its value becomes
  bypassable; readers wait. Reported by producer class.
* **Freezes**: integer divide = 2 + 4 x significant dividend bytes; fdiv +17,
  fsqrt +27, fcvt-to-int +1.

Parameters (CFG = `configs/os25.cfg`, GEN = `generate_vexii.sh`, VX =
`VexiiRiscv/src/main/scala/vexiiriscv`, SDR = `targets/pocket/io_sdram.v` in
the openfpgaOS repository):

| param | value | source |
|---|---:|---|
| ic_sets / ic_ways / ic_line | 256 / 2 / 64 | CFG:18; GEN:154; VX/Param.scala:885 |
| ic_nl_prefetch, pen_prefetch_cmd | 1, 1 | GEN:158; VX/fetch/Prefetch.scala:27-54, FetchL1Plugin.scala:385 |
| ic_redo | 4 | **GUESS**: REDO trap at fetch ctrlAt=3 |
| dc_sets / dc_ways / dc_line | 1024 / 2 / 64 | CFG:19; GEN:101; VX/…/LsuL1Plugin.scala:86 |
| sb_slots / sb_ops | 2 / 16 | GEN:102-103 |
| dc_redo | 9 | VX/…/LsuPlugin.scala:775-781 |
| btb_sets / hash / gshare / hist / ras | 256 / 16 / 4096 / 12 / 8 | CFG:29-30; GEN:164; VX/Param.scala:191,832,843 |
| pen_btb_taken / pen_mispredict | 2 / 8 | **Approximation** from VX/Param.scala:836, 968; omits fetch/execute overlap |
| lat_load / lat_mul / lat_div | 2 / 2 / 1 | VX/…/LsuPlugin.scala:75; MulPlugin.scala:47; DivPlugin.scala:36 |
| lat_csr | 1 | **GUESS** |
| lat_f2i / lat_fcmp | 2 / 1 | VX/…/FpuF2iPlugin.scala:22; GEN:168 (**GUESS** for cmp/class/mv) |
| lat_fadd / lat_fmul / lat_fdiv | 6 / 4 / 1 | GEN:169-170, FpuMulPlugin.scala:17; **+1 packer GUESS**; fdiv **GUESS** |
| lat_fma | 9 | Measured os25 dependent FMA interval: 10 cycles including the issue cycle; `openfpgaOS/tools/check_pocket_cpu_latency.py`, 2026-09-19 |
| lat_fmisc / lat_fload / lat_i2f | 2 / 2 / 2 | VX/Param.scala:1235; rest **GUESS** |
| div_base / div_per_byte | 2 / 4 | VX/…/DivPlugin.scala:115-127; DivRadix.scala:88-106 (CFG:35 DIV_RADIX=4) |
| fdiv_extra / fsqrt_extra / f2i_extra | 17 / 27 / 1 | FpuDivPlugin.scala:31-32, FpuSqrt.scala:33, FpuF2iPlugin.scala:108-111 (**+control GUESS**) |
| sdram_setup / per_word / row_miss | 11 / 2 / 5 | **GUESS** 4 (AXI fabric) + SDR:571,671,109; SDR:907-934; SDR:112-115 |
| sdram_wr_setup | 8 | **GUESS** |
| bram_setup / bram_per_word | 3 / 1 | **GUESS** |
| io_access_base | 3 | VX/…/LsuPlugin.scala:611-633 |
| io_bram_extra / io_mmio_extra / io_cram_extra / io_sdram_uc_extra | 1 / 2 / 10 / 13 | **GUESS** |
| misaligned_trap_cycles | 400 | **GUESS**: trap entry + decode + access + mret |

Contention from the GPU, audio and scanout on the shared SDRAM arbiter is not
modelled, and neither is the kernel's own code. All parameters live in
`qsim_model_params` and can be overridden through
`qsim_enable_profiling(cpu, &params)`.

### Known CPU-model deviations

* qsim is exact IEEE-754; the device FPU ignores subnormals.
* `mstatus.FS=0` does not make F instructions illegal; `wfi` is a no-op;
  `fence.i` does not flush the modelled I-cache.
* Legality follows the spec where VexiiRiscv is lenient (reserved `rm`, RV32
  `slli/srli/srai` with bit 25). Compilers never emit these.
* The LR/SC reservation is cleared only by SC and reset.
* Interrupts are recognized at defined check points, not at every instruction
  boundary. os25 apps run with interrupts off.
* Branch history is non-speculative; the store buffer is simplified.

---

## Tests

```sh
make cpu-test                          # tests/cpu/run_tests.sh
make gputest && ./gpu_equiv.sh 61000 10   # GPU model vs Verilator RTL, all traffic modes
./gpuvec_check.sh <run-dir>            # replay real captured frames through RTL + C model
```

The CPU suite needs guest ELFs cross-compiled once
(`make -f Makefile.cpu guests`, which uses the SDK container) and a musl
reference (`tests/cpu/build_muslref.sh`). Set `QSIM_TEST_APP_ELF` to a
production app ELF to add two more checks: a decoder cross-check over every
instruction of its functions, and its soft-float/libm helpers against the
host.

What the suite covers: the FPU against a GMP exact-rational reference across
five rounding modes (52 M checks); 876 generated instruction stubs over every
opcode class in both 32-bit and compressed encodings (4.4 M checks); a decoder
cross-check against `objdump -M no-aliases` over every 16-bit pattern and 250k
random words; budget/SMC/hook/alias/trap/interrupt API behaviour; and two
compiled C programs whose output must match the host bit-for-bit in both the
fast and the profiling interpreter.

---

## Reference

* **ENV_SPEC.md** — the emulated environment in full, fact by fact, with
  provenance tags ([ELF] / [SRC] / [DER] / [CHOICE]).
* **apps/quake/README.md** — the worked example: how the Quake port is
  profiled and gated, and the measurements taken with it.
* The openfpgaOS repository is the source of truth for the kernel, the caps
  table, the GPU RTL and the VexiiRiscv configuration this model follows.
