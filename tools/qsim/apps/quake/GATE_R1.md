# GATE_R1 — round-1 optimisation patches through the hardened qsim gate

> **Preserved measurement report.** Produced with qsim before it moved into the
> SDK (`tools/qsim/`), during the 2026-09 Quake optimization round. Absolute
> paths and `runs/`, `analysis/` and `review/` directory references below point
> at that session's scratch directories and no longer exist; the ELF sha256s,
> the workload definitions and the numbers do. The patches this report is about
> live in the Quake port repository under `tools/quake-opt-2026-09/`. Reproduce
> any of it with `tools/qsim/run.sh` / `gate.sh` (see `apps/quake/README.md`).

Date 2026-09-15. Gate binary `qsim` sha256 `352f38c9ef9f6b25c3fbd410ea6ee47c8c691f241815d61ed18ecb7d20c0ef26`
(built from the tree as found; `make -s qsim` is a no-op, so the sources were already consistent —
no qsim source was modified for this run).
Reference ELF: `/tmp/claude-1000/-home-alberto-Repos-Quake/9b01277e-d622-46dc-8403-c4a6d70cf27d/scratchpad/qopt/elfs/baseline.elf`
sha256 `0d7bd13e1721e62c78a3d5e03792d81d115006de8584234849b0197c6894b546`
(byte-identical to `/home/alberto/Repos/Quake/build/opt-20260915/baseline-obj/app.elf`).

All artefacts: `/tmp/claude-1000/-home-alberto-Repos-Quake/9b01277e-d622-46dc-8403-c4a6d70cf27d/scratchpad/qopt/gate-r1/`.

---

## 0. Verdict

| | ELFs |
|---|---|
| **PASS** (9) | `world-edge`, `alias-raster`, `audio`, `entities`, `server-model`, `client-view`, `frame-loop-exact`, `qc-vm`, `small-exact` |
| **FAIL** (3) | `gpu-iface-v2`, `world-surf`, `combined-r1b` |

600 gate runs (12 candidates x 2 ELFs x 5 workloads x 5 modes), plus 65 `--profile` runs
(13 ELFs x 5 workloads), 19 diagnostic runs and 18 PPM-capture runs. Every single run reached its terminator
(`timedemo_complete` for demo1/demo2/demo3/fx2, `workload_complete` for play1); no run errored,
timed out or lost a frame, and no run pair had a frame-count mismatch anywhere in the matrix.

**None of the three failures is an independent rendering defect introduced by the patch.**
All three land on one pre-existing latent bug in the *baseline*: `R_DrawParticles` z-tests against a
partially rasterised `d_pzbuffer` (section 4). Section 5 says what to do about each.

---

## 1. What was run

```
cd /tmp/claude-1000/-home-alberto-Repos-Quake/9b01277e-d622-46dc-8403-c4a6d70cf27d/scratchpad/qsim
./gate.sh ../qopt/elfs/baseline.elf ../qopt/elfs/<cand>.elf ../qopt/gate-r1/<cand>
```
driven by `gate-r1/run_gates.sh` (the `A` side is generated once and copied, stamps reused;
21-37 s per candidate, 5 min total). Profiles: `gate-r1/run_profiles.sh`
(`--profile --heap-size 0x2f5fb70`, default eager/flat). Both ELFs in every pair run with
`caps.heap_size` pinned to the baseline's device value `0x2f5fb70`, so a `.bss` change in B cannot
move the Quake heap or the surface cache.

- Workloads: `demo1` (e1m3 timedemo), `demo2` (e1m4), `demo3` (e1m6), `play1` (live e3m1 game,
  635-frame fight window + UI + intermission), `fx2` (demo2 with `r_waterwarp 1
  r_drawparticles 1 r_dynamic 1 d_mipscale 1.5` — all four off/at default on the device).
- Modes: `eager-lint`, `lazy-lint`, `eager-strict`, `lazy-strict`, `eager-seed`
  (`el ll es ls sd` in the tables below).

Raw reports: `gate-r1/<cand>/gate.txt` and `gate-r1/<cand>.out`; runs in
`gate-r1/<cand>/{A,B}/<workload>.<mode>/`; profiles in `gate-r1/profiles/<elf>.<workload>/`;
machine-readable roll-ups in `gate-r1/matrix.json` and `gate-r1/profdelta.json`.

This independently reproduces the Harden-B agent's r1 result (same 9/3 split, same failing runs,
same frame lists, same hazard counts) with a freshly generated `A` side.

---

## 2. PASS/FAIL matrix

Order within a workload cell: `eager-lint lazy-lint eager-strict lazy-strict eager-seed`.

| ELF | demo1 | demo2 | demo3 | play1 | fx2 | gate |
|---|---|---|---|---|---|---|
| `world-edge` | P P P P P | P P P P P | P P P P P | P P P P P | P P P P P | PASS |
| `gpu-iface-v2` | P P P P P | P P P P P | P P P P P | **F** P **F** P **F** | **F** P **F** P **F** | **FAIL** |
| `alias-raster` | P P P P P | P P P P P | P P P P P | P P P P P | P P P P P | PASS |
| `audio` | P P P P P | P P P P P | P P P P P | P P P P P | P P P P P | PASS |
| `entities` | P P P P P | P P P P P | P P P P P | P P P P P | P P P P P | PASS |
| `world-surf` | P P P P P | P P P P P | P P P P P | P P P P P | **F** P **F** P **F** | **FAIL** |
| `server-model` | P P P P P | P P P P P | P P P P P | P P P P P | P P P P P | PASS |
| `client-view` | P P P P P | P P P P P | P P P P P | P P P P P | P P P P P | PASS |
| `frame-loop-exact` | P P P P P | P P P P P | P P P P P | P P P P P | P P P P P | PASS |
| `qc-vm` | P P P P P | P P P P P | P P P P P | P P P P P | P P P P P | PASS |
| `small-exact` | P P P P P | P P P P P | P P P P P | P P P P P | P P P P P | PASS |
| `combined-r1b` | P P P P P | P P P P P | P P P P P | **F** P **F** P **F** | **F** P **F** P **F** | **FAIL** |

The failures are confined to the *eager* modes, and there all three eager modes agree with each
other on every candidate and workload (verified across the whole matrix): the failing frame sets are
identical in `eager-lint`, `eager-strict` and `eager-seed`, so neither the split-memory model nor the
seeded uninitialized memory adds or removes a divergence here. `lazy-*` PASSes because compare.py's
reference-race rule excludes exactly the frames the baseline itself renders differently under a
non-eager schedule — and every failing frame is in that set (section 4). `eager-seed` passing
everywhere also means no candidate reads memory nobody ever wrote.

---

## 3. Cost: est_cycles and insns vs baseline

Measurement window only (timedemo window for demo1/2/3/fx2; the `QSIM_WINDOW..QSIM_WINDOW_END`
fight window for play1). `*` = the est_cycles delta is inside the layout noise band from
`qsim/noise/noise_floor.json` (address-only variants: demo1 ±0.59 %, demo2 ±0.60 %, demo3 ±0.59 %,
play1 ±0.64 %, fx2 ±0.56 %), i.e. not distinguishable from moving code and data around.

### est_cycles delta

| ELF | demo1 | demo2 | demo3 | play1 | fx2 |
|---|---|---|---|---|---|
| `world-edge` | -2.82 % | -3.10 % | -3.05 % | -1.03 % | -2.73 % |
| `gpu-iface-v2` | -4.20 % | -4.09 % | -4.15 % | -4.83 % | -3.77 % |
| `alias-raster` | -1.72 % | -1.50 % | -1.88 % | -1.14 % | -1.36 % |
| `audio` | -2.65 % | -2.86 % | +0.04 % `*` | -0.69 % | -2.58 % |
| `entities` | -2.51 % | -0.87 % | -1.76 % | -4.50 % | -0.79 % |
| `world-surf` | -0.53 % `*` | -0.52 % `*` | -0.40 % `*` | -0.31 % `*` | -0.41 % `*` |
| `server-model` | +0.23 % `*` | +0.34 % `*` | +0.56 % `*` | **-8.17 %** | +0.31 % `*` |
| `client-view` | -3.85 % | -2.53 % | -3.00 % | -5.49 % | -2.29 % |
| `frame-loop-exact` | +0.08 % `*` | +0.05 % `*` | +0.08 % `*` | -0.01 % `*` | +0.04 % `*` |
| `qc-vm` | -0.45 % `*` | -0.54 % `*` | -0.50 % `*` | -1.90 % | -0.44 % `*` |
| `small-exact` | -3.09 % | -2.59 % | -3.29 % | -2.91 % | -2.32 % |
| `combined-r1b` | **-21.82 %** | **-19.93 %** | **-19.85 %** | **-28.28 %** | **-18.04 %** |

### insns delta

| ELF | demo1 | demo2 | demo3 | play1 | fx2 |
|---|---|---|---|---|---|
| `world-edge` | -3.69 % | -3.98 % | -4.16 % | -1.21 % | -3.46 % |
| `gpu-iface-v2` | -4.86 % | -4.80 % | -5.05 % | -5.56 % | -4.38 % |
| `alias-raster` | -2.02 % | -1.65 % | -2.43 % | -1.35 % | -1.44 % |
| `audio` | -3.59 % | -4.02 % | -1.27 % | -1.02 % | -3.51 % |
| `entities` | -3.21 % | -1.19 % | -2.35 % | -6.11 % | -1.04 % |
| `world-surf` | -0.56 % | -0.68 % | -0.53 % | -0.22 % | -0.59 % |
| `server-model` | -0.68 % | -0.50 % | -0.52 % | **-10.13 %** | -0.43 % |
| `client-view` | -5.00 % | -3.29 % | -4.04 % | -7.45 % | -2.88 % |
| `frame-loop-exact` | +0.03 % | +0.03 % | +0.03 % | +0.01 % | +0.03 % |
| `qc-vm` | -0.00 % | -0.00 % | -0.01 % | -2.03 % | -0.00 % |
| `small-exact` | -4.31 % | -3.63 % | -4.67 % | -3.65 % | -3.18 % |
| `combined-r1b` | **-25.64 %** | **-22.85 %** | **-23.29 %** | **-34.79 %** | **-20.20 %** |

Baseline absolutes (for scale):

| workload | window | frames (window/run) | insns | est_cycles |
|---|---|---|---|---|
| demo1 | timedemo | 969/1000 | 1,799,169,352 | 3,047,172,563 |
| demo2 | timedemo | 985/1016 | 1,748,415,337 | 2,976,477,818 |
| demo3 | timedemo | 1090/1121 | 2,020,409,672 | 3,391,254,255 |
| play1 | marker (fight) | 635/1034 | 1,767,388,414 | 3,160,714,810 |
| fx2 | timedemo | 985/1016 | 1,996,202,386 | 3,312,702,038 |

Reading notes:

- `server-model` is the only cluster whose value is workload-shaped: it does essentially nothing on
  a timedemo (+0.2…+0.6 %, inside noise) and is the second-largest win on `play1` (-8.17 % cycles,
  -10.13 % insns), because its targets (`SV_RecursiveHullCheck`, `SV_Move`, `MSG_ReadCoord`,
  `SV_Physics`) only run when a server is simulating. A demo-only benchmark would have rejected it.
- `qc-vm` likewise: -0.45 % on demos (inside noise, and insns are literally unchanged: -0.00 %)
  against -1.90 %/-2.03 % on play1. Its demo-side est_cycles win is cache-layout, not work removed.
- `entities` and `client-view` also gain noticeably more on play1 than on the demos.
- `frame-loop-exact` is a no-op by design (+0.03 % insns everywhere) — it exists to prove that the
  non-exact `frame-loop` change is what broke the oracle, not the rest of that cluster.
- `audio` on demo3 lands at +0.04 % cycles (inside noise) although insns drop 1.27 %; the removed
  work is uncached stores, which the timing model charges outside est_cycles' cache accounting.
- `world-surf`'s cycle win (-0.4…-0.5 %) is inside the noise band on every workload; its insn win
  (-0.5…-0.7 %) is real but small. Its value is elsewhere: it more than halves the baseline's
  `fx2` coherency-hazard count (section 6).
- All absolute est_cycles figures here are 0.67 % lower than `BASELINE_PROFILE.md` and the round-1
  per-cluster reports because of Harden A's `cbo` timing-model correction. A/B *deltas* are
  unaffected; absolute numbers from before that change should not be mixed with these.

---

## 4. The three failures

### 4.1 Common root cause (all three): the particle z test races the GPU world fill

`r_main.c:1191-1195`:

```c
	if (pq_gpu_zwrite_pending)
	{
		of_emit_finish();
		pq_gpu_zwrite_pending = 0;
	}

	if (r_drawparticles.value)
		R_DrawParticles ();
```

The drain that makes the GPU-written depth buffer visible before the particle pass is **conditional
on `pq_gpu_zwrite_pending`**, a flag that `d_scan.c:1232` sets only when a surface took the Q29
combined-z path in full (`submitted_records == total_records`), that `d_sprite.c:152` also sets, and
that four intermediate consumers clear when they self-drain (`d_polyse.c:220`, `d_zpoint.c:40`,
`d_sprite.c:234`, `d_part.c:47`). It is therefore not a conservative cover for "the GPU still has
queued z writes". `D_DrawParticle` (`d_part.c:94`) then does `pz = d_pzbuffer + d_zwidth*v + u` and
z-tests a buffer that is only partly rasterised. Which particles survive depends on how much GPU
work happened to be *published* at that instant.

Evidence, all on the **unmodified baseline ELF**:

1. `fx2` `eager-lint` and `lazy-lint` of the same baseline ELF disagree on **24 frames**:
   `2 93 94 95 96 97 98 109 110 111 764 765 766 767 768 769 770 821 839 841 843 844 845 940`.
   (`gate-r1/divframes.py world-surf/A/fx2.eager-lint world-surf/A/fx2.lazy-lint`)
2. **Every** divergent frame of all three failing candidates is inside that set.
3. Turning particles off (`+r_drawparticles 0`, everything else in fx2 unchanged) makes all three
   candidates render **all 1016 fx2 frames identically** to the baseline — 0 px, 0 z, 0 pal —
   and collapses the baseline's own eager-vs-lazy inconsistency from 24 frames to 1
   (frame 2, the documented conback boot race).
   (`gate-r1/fx2_noparticles.sh`, runs in `gate-r1/diag/*.fx2nop.*`)
4. The pixel diffs are particle-shaped: 4x4 / 2x2 / 1x2 axis-aligned blocks in warm palette colours
   over a dark wall, exactly `rows = pix << d_y_aspect_shift` with `pix` in 1..4.
   The baseline eager-vs-lazy diff at fx2 frame 844 is a single clean 4x4 block at
   x 146..149, y 135..138, colour `(135,3,3)` present in eager and absent in lazy.

`hash_z` is identical on every frame of every run — the *final* depth buffer always agrees. Only the
mid-frame read is racy. qsim's memory model does **not** classify this as a coherency hazard (with
`r_waterwarp 0` the baseline reports 0 hazards while still being schedule-dependent); it is an
ordering bug, and only the pixel oracle plus the multi-schedule comparison catch it.

> **This is a real device bug, not a simulator artefact.** On hardware the GPU is neither
> infinitely fast nor infinitely lazy, so which particles appear depends on real GPU/CPU timing.
> It is invisible today only because `r_drawparticles` is 0 in the shipped configuration.
> Suggested fix: make the pre-particle drain unconditional (or set `pq_gpu_zwrite_pending`
> whenever any GPU command that writes z is queued). One line, and it un-blocks two clusters.

### 4.2 `gpu-iface-v2`

| run | divergence |
|---|---|
| `fx2.eager-lint` / `.eager-strict` / `.eager-seed` | `hash_px` on **8 frames**, first **94**; frames `94 97 769 770 841 843 844 845` |
| `play1.eager-lint` / `.eager-strict` / `.eager-seed` | 0 px / 0 z / 0 pal; **new hazard class** `gpu_read_of_uncommitted_cpu_store`, 160 events, A has 0 |
| everything else (demo1-3, all lazy runs) | PASS |

**Pixel diffs** (`gate-r1/ppm/`, `ppmdiff.py`; PPM frame index verified against `frames.csv`):

| frame | differing px | bbox | shape |
|---|---|---|---|
| 94 | 9 | x 153..164, y 117..132 | 9 isolated px, grey/blue -> red `(150,3,3)`/`(157,3,3)` |
| 97 | 2 | x 143, y 148..149 | 1x2 |
| 769 | 18 | x 177..180, y 145..150 | red `(143,3,3)` in A replaced by wall in B |
| 770 | 2 | x 165, y 127..128 | 1x2 |
| 841 | 32 | x 156..161, y 143..150 | two 4x4 blocks -> `(208,149,81)` |
| 843 | 5 | x 167..176, y 144..145 | |
| **844** | **20** | x 171..177, y 133..142 | 4x4 at (171,133) + 2x2 at (176,141), `(29,29,36)` -> `(196,132,79)` |
| 845 | 8 | x 155..156, y 128..131 | 2x4 |

Always inside the 3D view (`y < 176`), never the status bar, never more than 32 of 64000 pixels.

**Most likely responsible sub-change: `streaming-affine-lanes`** (with `private-param-emitter`
contributing). `of_emit_span` no longer copies span descriptors into
`span_buf[PQ_GPU_SPAN_BATCH_MAX]` for later encoding at `flush_span_batch()`; it encodes each span
straight into the GPU command staging buffer (`gpu_stream_affine_span` / `gpu_stream_persp_span`,
`patches/gpu-iface-v2.patch:3902-3915` equivalent hunk). Because `of_gpu_kick()` publishes as soon
as `_gpu_cmd_words >= OF_GPU_KICK_MIN_WORDS` (512, `of_gpu.h:1045-1050`), moving the encode earlier moves
the *publish* earlier, so a different amount of the world z fill has run when `R_DrawParticles`
reads `d_pzbuffer`. Measured on fx2 `eager-lint`: publishes 7921 -> 8174, published words
8,645,185 -> 7,903,310, fence-read barriers 1895 -> 2905, kernel-wait barriers 1010 -> 0.

Note the v1 -> v2 delta already removed the explicit `if (_gpu_cpu_ring) of_gpu_kick();` at the end
of `gpu_param_span_list` (the `gpu-publish-early` part-1 that made v1 fail demo1 frame 877, with the
new comment "publishing during the world pass changes pixels where D_SCAlloc later reuses a queued
surface-cache block"). That fixed the demo1 regression; the remaining fx2 divergence comes from the
implicit 512-word threshold, which the patch cannot avoid while it streams. `of_emit_publish()` is
defined but never called in this ELF, and the one `of_gpu_kick_now()` call site is the VID_Update
CMD_FLIP path (a frame boundary — harmless).

**The play1 hazard is benign false sharing, and is a separate issue from the fx2 failure.**

- Exactly 4 events per frame on frames **809..848** (40 consecutive frames), 160 total; A has 0.
- Sites: `flush_span_batch.part.0.lto_priv.0+0xa92` (100) and
  `_gpu_flush_cmd_stream.lto_priv.0+0x2d2` (60). Dirtied by `R_RenderView_+0x22a`.
- Address `0x1048178c` is `r_framecount`; the 64-byte line `0x10481780` starts at
  `draw_fade_pattern` (8 bytes, `draw.c:42`, `__attribute__((aligned(64)))` but **not padded** to a
  line) and also holds `r_pixbytes`, `ylast`, `xlast`, `_gpu_publish_uncached`, `raw_pairs`,
  `l2_slot`, `r2_slot`, `select_slot`, `y_slot`.
- `draw_fade_pattern` is the texture source of the `Draw_FadeScreen` spans (`draw.c:949`), cleaned
  once at init (`draw.c:193`) and never written again. Frames 809-848 are the menu: at frame 820,
  74.7 % of the `(x&3)!=t` fade-mask pixels are black, i.e. `Draw_FadeScreen` is running.
- So the GPU reads a line that the CPU holds dirty *because of a neighbouring variable*, and the
  eight bytes it actually uses are identical in both copies. Confirmed by `--mem strict`, where the
  GPU reads committed SDRAM: `play1.eager-strict` has **0** pixel differences over all 1034 frames.
- In the baseline's LTO `.data` order `draw_fade_pattern`'s line holds only menu/network statics, so
  it is never dirty at GPU-read time. None of the 7 layout variants in `qsim/noise/elfs/` reproduce
  the pairing either, so this cannot be dismissed as pure layout noise — but it is also not a data
  hazard. The proper fix is in `draw.c`: pad `draw_fade_pattern` to a full 64-byte line (it is a
  GPU-sampled texture and must own its line), which removes the class regardless of LTO ordering.

### 4.3 `world-surf`

| run | divergence |
|---|---|
| `fx2.eager-lint` / `.eager-strict` / `.eager-seed` | `hash_px` on **1 frame**, frame **844** |
| everything else (demo1-3, play1, all lazy runs) | PASS |

**Pixel diff, fx2 frame 844: 20 pixels**, bbox x 171..177 y 133..142 — a 4x4 block at (171,133)
and a 2x2 at (176,141), `(29,29,36)` -> `(196,132,79)`. **Byte-for-byte the same 20 pixels as
`gpu-iface-v2`'s frame 844**, i.e. the same particle.

**Most likely responsible sub-change: `scratch-bump-per-frame`.** `D_AllocTurbScratchRow()` (128
fixed 1280-byte rows) became `D_AllocTurbScratch(nbytes)`, a 64-byte-granular bump allocator over
the same 163,840-byte arena, plus the shared `d_scratch_epoch` that lets a drain in one pool reset
the others (`patches/world-surf.patch`, `d_scan.c` hunk at `D_AllocTurbScratch`, `draw.c` hunk for
the text/translate pools). At the 320-pixel view width the arena now holds 512 allocations instead
of 128, so the `of_emit_finish(); of_emit_texture_cache_flush();` on exhaustion — which is a **full
GPU drain** — fires roughly a quarter as often. `Turbulent8` allocates from the same pool during
the world pass, *before* `R_DrawParticles`, so removing those drains changes how much GPU z has
retired at the particle z test. Measured on fx2 `eager-lint`: publishes **7921 -> 7006** (-915 over
1016 frames, i.e. about one lost drain per frame) and fence-read barriers **1895 -> 1232** (-663);
`published_words` is essentially unchanged (8,645,185 -> 8,642,924), so this is purely a
*when*-not-*what* change.

Decisive attribution: with `+r_waterwarp 0` (particles still on, everything else identical),
`world-surf` renders **all 1016 fx2 frames identically** to the baseline, while `gpu-iface-v2` and
`combined-r1b` still diverge on 6 frames. The water-warp scratch pool is therefore necessary for
world-surf's failure and irrelevant to the other two. (`gate-r1/diag/*.nowarp.*`)

Everything else in this cluster is clean: `d_mipscale_tab` (exact), the `D_Q29BeginFrame` per-frame
latch of `d_q29_params` (semantically identical — `flags` is the constant `OF_EMIT_PERSP`, so the
dropped `(flags & (SKIP_ZERO|COLUMN)) == 0` term was always true), the single-walk record build with
the `MAXSPANS` array, `d_sprite_param_records` alignment, and the integer `D_SpritePlaneFixed`
emulation. All of demo1, demo2, demo3 and play1 pass in all five modes.

### 4.4 `combined-r1b`

| run | divergence |
|---|---|
| `fx2.eager-*` | `hash_px` on **6 frames**, first **94**; frames `94 97 764 768 844 940` |
| `play1.eager-*` | 0 px; **new class** `gpu_read_of_uncommitted_cpu_store`, 160 events, same two sites |
| everything else | PASS |

`combined-r1b` = every round-1 cluster with `frame-loop-exact` in place of `frame-loop`
(the only file-list difference from `combined-r1` is `main.c`). It carries the `gpu-iface-v2` span
streaming (`span_pending` / `gpu_stream_*` / `of_emit_publish`, `patches/combined-r1b.patch:3364-3915`)
and the `world-surf` scratch pool, and it inherits **both** failure modes and nothing new:

- play1: the identical 160-event false-sharing hazard, same frames 809..848, same two sites, with
  `draw_fade_pattern` at `0x10481980` sharing its line with `r_framecount` at `0x1048198c`.
- fx2 pixel diffs, all particles again:
  - frame 94: 9 px, x 153..164 y 117..132 — **identical to `gpu-iface-v2`'s frame 94**
  - frame 97: 2 px, x 143 y 148..149 — identical to `gpu-iface-v2`
  - frame 764: 3 px, x 156..167 y 128..129
  - frame 768: 28 px, x 170..174 y 130..150 — a particle `(220,132,74)` present in A, absent in B
  - frame 844: 19 px, x 171..177 y 133..142 — the same particle as world-surf/gpu-iface-v2 (one
    pixel fewer)
  - frame 940: 26 px, x 153..170 y 134..149
- Also reported (as a *note*, not a failure) on fx2: `writeback_reverts_gpu_or_uncached_write`
  488 -> 589 with sites not in A (`CL_ParseUpdate`, `D_CacheSurface`, `IN_SendKeyEvents`,
  `SCR_UpdateScreen.part.0`). This class is an advisory worst-case counter and moves with layout.

Measured publish change on fx2: 7921 -> 7276 publishes, 8,645,185 -> 7,901,049 words,
fence-read barriers 1895 -> 2242, kernel-wait 1010 -> 0 — the two effects superposed.

---

## 5. Recommendation per ELF

| ELF | verdict | action |
|---|---|---|
| `world-edge` | PASS, -2.8…-3.1 % cycles | land |
| `alias-raster` | PASS, -1.4…-1.9 % | land |
| `audio` | PASS, -2.6…-2.9 % on demo1/2/fx2, noise on demo3 | land |
| `entities` | PASS, -0.8…-4.5 % | land |
| `server-model` | PASS, **-8.2 % on play1**, noise on demos | land; benchmark it on play1, not on a timedemo |
| `client-view` | PASS, -2.3…-5.5 % | land |
| `frame-loop-exact` | PASS, ±0.08 % (no-op) | land if you want the structural change; no measurable win |
| `qc-vm` | PASS, -1.9 % play1, noise on demos | land; play1 is its benchmark |
| `small-exact` | PASS, -2.3…-3.3 % | land |
| `world-surf` | FAIL on 1 fx2 frame | the patch is **not** wrong; it removes drains that were accidentally papering over the `R_DrawParticles` race. Fix `r_main.c:1191` first, then re-gate. Independent of that, this cluster alone halves the fx2 hazard count (180,943 -> 83,018) and cuts the lazy-mode boot hazards 256 -> 192. |
| `gpu-iface-v2` | FAIL on 8 fx2 frames + a new play1 hazard class | two separable items: (a) the fx2 frames are the same `r_main.c:1191` race — fix that and re-gate; (b) pad `draw_fade_pattern` to 64 bytes in `draw.c` (it is a GPU texture source that currently shares a line with `r_framecount`), which removes the play1 hazard class for good. Both fixes are in files this cluster does not own. |
| `combined-r1b` | FAIL (inherits both) | re-gate after the two fixes above; nothing new is wrong with the combination itself. |

If the two upstream fixes are made, the expected outcome is a clean sweep and a **-21.8 % est_cycles
/ -25.6 % insns** demo1 win (`-28.3 % / -34.8 %` on the play1 fight window) from `combined-r1b`.
Until then the safe combined set is `combined-r1b` minus `gpu-iface-v2` minus `world-surf`
(worth roughly -17 % cycles on demo1 by summation of the passing clusters, un-measured as a build).

---

## 6. Hazard deltas (whole matrix)

`A -> B` totals. The `demo1-3 + play1` columns collapse identical values across those four
workloads; where a cluster differs on one of them, both values are shown.

| ELF | demos+play1, eager | demos+play1, lazy | fx2 eager | fx2 lazy | new class |
|---|---|---|---|---|---|
| `world-edge` | 0->0 | 256->256 | 180943->185393 | 195952->201239 | no |
| `gpu-iface-v2` | 0->0, **0->160** (play1) | 256->256, 256->328 (play1) | 180943->194395 | 195952->216285 | **`gpu_read_of_uncommitted_cpu_store`** (play1) |
| `alias-raster` | 0->0 | 256->256 | 180943->182391 | 195952->198624 | no |
| `audio` | 0->0 | 256->256 | 180943->217998 | 195952->233925 | no |
| `entities` | 0->0 | 256->256 | 180943->167610 | 195952->168508 | no |
| `world-surf` | 0->0 | **256->192** | **180943->83018** | 195952->93119 | no |
| `server-model` | 0->0 | 256->256 | 180943->173869 | 195952->178162 | no |
| `client-view` | 0->0 | 256->256 | 180943->206056 | 195952->219887 | no |
| `frame-loop-exact` | 0->0 | 256->256 | 180943->188238 | 195952->202441 | no |
| `qc-vm` | 0->0 | 256->256 | 180943->141929 | 195952->152767 | no |
| `small-exact` | 0->0 | 256->256 | 180943->179031 | 195952->195197 | no |
| `combined-r1b` | 0->0, **0->160** (play1) | 256->256, 256->356 (play1) | **180943->130250** | 195952->133804 | **`gpu_read_of_uncommitted_cpu_store`** (play1) |

Three things to read out of this table:

1. **demo1-3 and play1 are hazard-clean in every eager mode for 10 of 12 candidates** (0 -> 0).
   That is the meaningful signal, and only `gpu-iface-v2` / `combined-r1b` break it (section 4.2).
2. **The 256 lazy-mode hazards are the baseline's known boot race** — queued conback spans reading
   Quake-cache memory that `Cache_Alloc` memsets before the first barrier (`Draw_BeginDisc`, `draw.c:1002`), frame 2 of every workload. `world-surf` reduces it to 192.
3. **The fx2 counts are meaningless as a pass/fail signal and are entirely `r_waterwarp 1`.**
   Measured on the baseline (`gate-r1/diag/baseline.*`):

   | fx2 variant | baseline hazards |
   |---|---|
   | as shipped in the workload table | 180,943 |
   | `r_waterwarp 0` (particles, dlights, mip3 still on) | **0** |
   | `r_dynamic 0` | 217,943 |
   | `d_mipscale 0.3` (mip3 off) | 185,352 |
   | `r_drawparticles 0` | 183,596 |

   So the water-warp path is the sole source, and the mechanism is identified: when `r_waterwarp`
   is on, `d_viewbuffer = r_warpbuffer` (`d_init.c:139`) and `r_warpbuffer` is
   `byte warpbuffer[WARP_WIDTH*WARP_HEIGHT]` — a 64 KB **local array on `R_RenderView_`'s stack**
   (`r_main.c:1127-1129`). The GPU rasterises the world into it, then `D_WarpScreen` reads it back
   with ordinary cached loads (`row[turb[u]][col[u]]`, `d_scan.c`) and never invalidates those
   lines. Every example confirms it: reading pc `R_RenderView_+0x458` (`D_WarpScreen` inlined),
   `dirtied by 0x00000000` (= a GPU write, no CPU pc), addresses `0x13a7xxxx`, i.e. ~50 KB below
   qsim's `ENV_STACK_TOP` `0x13A80000`. Candidate counts swing from 83,018 to 217,998
   (-54 %…+20 %) purely on how the
   scratch/publish timing lands, which is why the gate compares hazard *classes and sites*, not
   counts, inside a class the reference already has. It also means **fx2's hazard column must not be
   used to rank patches** — only the "new class" column is a gate signal.

   Note that this storm changes **no pixels by itself**: with `r_drawparticles 0` and
   `r_waterwarp 1` all four ELFs render all 1016 frames identically. It is nonetheless a real
   device bug and a second candidate for an upstream fix.

---

## 7. Reproducing

```
G=/tmp/claude-1000/-home-alberto-Repos-Quake/9b01277e-d622-46dc-8403-c4a6d70cf27d/scratchpad/qopt/gate-r1
$G/run_gates.sh        # 12 gates, ~5 min on 16 cores        -> $G/<cand>/gate.txt, $G/summary.txt
$G/run_profiles.sh     # 65 --profile runs, ~3 min           -> $G/profiles/<elf>.<workload>/
python3 $G/profdelta.py                                      # section 3 tables
python3 $G/collect.py                                        # sections 2 and 6 tables -> $G/matrix.json
$G/run_captures.sh     # PPMs of the divergent fx2 frames    -> $G/ppm/
$G/fx2_noparticles.sh  # the r_drawparticles 0 control       -> $G/diag/
$G/divframes.py <runA> <runB>          # frames where hash_px/z/pal/cmd differ
$G/ppmdiff.py <a.ppm> <b.ppm>          # pixel count, bbox, rows/cols, first differing pixels
$G/capture.sh <elf> <out> <workload> <mode> <first> <last>   # PPMs for one frame range
```

`capture.sh` truncates the run with `--frames`; verified that every frame before the cut has
identical `hash_px` to the full gate run, and that PPM file numbering equals the `frames.csv`
frame index (checked at fx2 841-845: 841/843/844/845 differ, 842 identical, matching the CSV).

## 8. Limits of this gate run

- Everything here is qsim. Nothing was run on hardware. The known-unmodelled items are unchanged
  from Harden A/B: `acquire_next`'s bounded fence spin, D-cache writeback/store-buffer latency,
  kernel-side cached traffic and service CPU cost, SDRAM arbiter contention, and any real GPU pixel
  rate. `lazy` is a legal extreme, not a timing model.
- est_cycles deltas below about 0.6 % are inside the layout noise band and are marked `*`;
  the noise floor is a whole-run figure, not per function, and no confidence interval is attached
  to per-function deltas.
- `play1` is open-loop and bit-exact, so it cannot A/B a patch that perturbs server floating point.
  All 12 candidates here are physics-exact on play1 (0 px differences), which is itself a result.
- Hazard-count regressions *inside* a class the reference already has (all of fx2) are reported as
  notes and do not fail. A candidate that made the water-warp path worse would pass with a note.
- The sub-change attributions in section 4 are the best explanation consistent with the patch text
  and with the controlled experiments in `gate-r1/diag/` (particles on/off, waterwarp on/off,
  publish/barrier counters). They were **not** confirmed by building one-sub-change-at-a-time ELFs;
  that would need a rebuild per hypothesis.
