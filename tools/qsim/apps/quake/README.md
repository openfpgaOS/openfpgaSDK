# qsim app: Quake — the worked example

This directory is everything qsim needs to profile and A/B-gate the
openfpgaOS **Quake** port (`src/quake/engine` in a Quake port checkout). It is
also the reference for what an app directory looks like; see
`tools/qsim/README.md`, "Onboarding a new app", for the format.

```
apps/quake/
├── quake.app                 the app profile: data slots, .ini, argv[0], console conventions
├── quake.ini.in              the .ini the emulated kernel serves as slot 2 ($ARGS expanded)
├── workloads/
│   ├── workloads.sh          the five workloads, shared by run.sh and gate.sh
│   ├── play1.cfg             the live-game console script (generated, checked in)
│   ├── gen_play1.py          the generator that produced it — provenance, not a build step
│   ├── mapents.py            BSP entity-lump reader used by the generator
│   └── probe/                the map-selection experiments that picked e3m1
├── noise/
│   ├── build_variant.sh      builds functionally identical layout variants of an ELF
│   ├── patches/uninit.patch  a validation build that reads never-initialized memory
│   └── noise_floor.json      the measured layout noise floor (see below)
├── BASELINE_PROFILE.md       per-function profile of the baseline ELF
├── PROFILE_R1B.md            profile of the round-1 optimization candidates
└── GATE_R1.md                gate results for those candidates
```

The optimization patches these reports are about live in the Quake port
repository, under `tools/quake-opt-2026-09/`.

## Running it

```sh
cd tools/qsim
make
export QSIM_DATA_DIR=~/.local/share/Steam/steamapps/common/Quake/id1   # PAK0.PAK, PAK1.PAK

./run.sh baseline.elf out/demo1 demo1          # profile one workload
./gate.sh baseline.elf candidate.elf out/gate  # 5 workloads x 5 modes x 2 ELFs
```

`QSIM_APP=quake` is the default, so nothing else is needed. Any registered
Quake `id1` directory works — the PAK names are matched case-insensitively.
Shareware `pak0.pak` alone is not enough: the workloads use demos and maps
from the registered `PAK1.PAK`.

Direct invocation, for reference:

```sh
./qsim baseline.elf --app apps/quake/quake.app --data-dir "$QSIM_DATA_DIR" \
       --out out/demo1 --profile --args "+timedemo demo1 +host_framerate 0.1"
```

## What the profile declares

`quake.app` is the whole Quake-specific surface of the tool:

* **Data slots** — `os.bin` (1, stub), `quake.ini` (2, generated),
  `quake.elf` (3, stub with a *constant* size: the engine prints it to the
  console, so it must not vary between builds), `pak0.pak` (4) and `PAK1.PAK`
  (5) from `--data-dir`, `quake.cfg` (8, NV, starts empty — the engine writes
  its 20-byte marker), and `s0.sav`…`s9.sav` (10–19, NV). No CD tracks, which
  is why `CDAudio_DumpRoot` prints the registry as notify text on screen.
* **`build_stamp = "Exe: "`** — the engine prints `Exe: <__TIME__> <__DATE__>`,
  visible on the console in frames 0–11. qsim rewrites the date in the loaded
  image so two builds made at different times still render identically.
  `--no-normalize-build-stamp` turns that off (and then those frames diverge,
  which is how the mechanism was verified).
* **`frame_symbol = host_framecount`, `zbuffer_symbol = zbuffer_storage`** —
  `R_RenderView` queues the z clear for the *next* frame ahead of the flip, so
  qsim hashes the z-buffer just before that clear.
* **`result_line`** — `CL_FinishTimeDemo`'s `"<N> frames <T> seconds <F> fps"`.
  The N is the measurement window: the last N presented frames.
* **`parity`** — the `"35.0 megabyte heap"` and `"5120k surface cache"` lines.
  If these differ between two runs, the two ELFs did not run the same
  workload, and the comparison is meaningless.

## The five workloads

| workload | what it runs | why | frames | insns |
|---|---|---|---|---|
| `demo1` | `+timedemo demo1 +host_framerate 0.1` (e1m3) | the canonical profile workload | 1000 | 1.86 G |
| `demo2` | `+timedemo demo2` (e1m4, underwater) | palette shift, warp-adjacent paths | 1016 | 1.81 G |
| `demo3` | `+timedemo demo3` (e1m6, sky and lava) | the sky rasterizer, mip2 surface blocks | 1121 | 2.08 G |
| `play1` | a live single-player game on e3m1 (`workloads/play1.cfg`) | **server + QuakeC + loopback client**, physics, hull traces, UI, intermission | 1034 | 2.84 G |
| `fx2` | demo2 with `r_waterwarp 1 r_drawparticles 1 r_dynamic 1 d_mipscale 1.5` | the render paths that are off by default | 1016 | 2.05 G |

Device cvar defaults (`quake.ini` ships `ARGS=` empty; `quake.rc` runs
`default.cfg` plus the engine's `gamma 0.7`): `r_waterwarp 0`,
`r_drawparticles 0`, `r_dynamic 0`, `d_mipscale 0.3`, `d_mipcap 0`,
`r_worldtris 0` (the GPU-triangle world path "wedges the GPU on this
bitstream", `r_main.c:154`), `r_hwspan 1`, `r_hwzspan 1`, `r_gpuworld 1`,
`r_fasttex 1` (falls back to main memory on os25), `viewsize 100`, `skill 1`.
So `demo1`–`demo3` and `play1` run exactly what the device runs; `fx2` is the
only workload that turns things on, and a player can only reach those from the
console.

### `play1` — why a recording is not enough

`timedemo` plays back a recorded client stream: no server, no QuakeC, no
physics. `play1` runs the real thing (`map e3m1`, then a scripted player), so
`PR_ExecuteProgram`, `SV_Physics`, `SV_Move`, `SV_RecursiveHullCheck`,
`world.c` traces, `SV_SendClientMessages` and the loopback net driver all
execute — **655 M of the run's 2.84 G instructions (23%)**, none of which a
timedemo touches.

The script is plain console commands with `wait` (one host frame each), served
to the guest as data slot 20 by `--file play1.cfg=…`, and run from
`stuffcmds` via `+exec play1.cfg`. `--window-on QSIM_WINDOW`,
`--window-end-on QSIM_WINDOW_END` and `--end-on QSIM_END` (echo lines in the
script) delimit the measurement window and end the run. What happens: load
e3m1, `god`, `impulse 9`, a noclip flight to the deathmatch spot at
(616 −1128 −56) inside a group of grunts, enforcers and dogs, noclip off, then
**635 frames of fighting** with quad damage and six weapons while walking,
strafing, jumping and turning; then, outside the window, the UI (scoreboard,
`sizedown`/`sizeup`, main menu, options, player setup, help, console,
`screenshot`, `centerview`) and a second noclip flight into the level's
`trigger_changelevel`, which ends the run on the intermission screen.

Verified behaviour: monsters wake, chase and attack (`SV_MoveToGoal` 20,882
calls); the player kills 12 of the 55 monsters (the scoreboard and
intermission screen say so); rockets, nails and grenades fly
(`SV_Physics_Toss` 59,033 calls); gibs and corpses appear.

`gen_play1.py` regenerates the script. The first flight is arithmetic from the
BSP entity lump; the second starts wherever the fight left the player, so the
generator appends `edict 1` (which prints `origin` and `v_angle`), runs qsim,
reads the position and appends one flight leg, repeating until the player is
inside the exit trigger. Noclip movement is exact (velocity = wish velocity,
clamped to `sv_maxspeed` 320; a key counts 0.5 the frame it goes down, 1.0
while held, 0 when released), and the yaw reaches the server as one byte
(`MSG_WriteAngle`), which is why each leg re-measures.

**`play1` is open-loop**: the script is a fixed sequence of console commands,
so an optimization that changes server floating-point results (the engine is
built `-ffast-math`) diverges from the first changed physics frame and the
gate reports it as a rendering difference. That is the intended semantics —
bit-exact or not equivalent — but it makes `play1` the most sensitive
workload.

## Measured results (baseline ELF `0d7bd13e…b546`, host Ryzen AI MAX+ 395)

**demo1, `--profile`**: 1,858,166,272 guest instructions and 3,173,158,707
est_cycles over 1000 presented frames (31 console/loading flips, then 969
timedemo frames). Boot to the first flip is 4.78 M insns. Timedemo window
per-frame insns p50 1,817,777 / p90 2,617,835 / max 6,380,194; est_cycles p50
3,097,964 / p90 4,460,830 / max 8,765,371.

Top self cycles: `R_GenerateSpans_Array` 8.2%, `R_ClipEdge` 7.6%,
`flush_span_batch` 6.3%, `R_RecursiveWorldNode` 5.4%, `__muldf3` 4.8%,
`R_RenderFace` 4.7%, `R_ScanEdges` 4.3%, `D_PolysetDraw` 4.2%, `D_DrawSpans8`
4.1%, `R_AliasDrawModel` 4.1%. Soft-float helpers are 13.9% of cycles over
4.93 M calls, mostly from `R_AliasDrawModel`, `R_ClipEdge`, `S_Update`,
`R_RecursiveWorldNode` and the double-precision `__sindf`/`__cosdf`. Full
tables in **BASELINE_PROFILE.md**.

**Determinism**: two fast runs give byte-identical `frames.csv`,
`console.log` and `summary.json` (apart from wall time and MIPS); profile runs
agree with fast runs on every mode-independent column; all seven layout
variants render every workload identically.

**Layout noise floor** (`noise/noise_floor.json`, seven functionally identical
builds):

| workload | est_cycles band (address-only variants) | including the alignment builds |
|---|---|---|
| demo1 | ±0.59% | ±0.95% |
| demo2 | ±0.60% | ±1.17% |
| demo3 | ±0.59% | ±1.07% |
| play1 | ±0.64% | ±0.64% |
| fx2 | ±0.56% | ±0.98% |

The address-only variants execute exactly the same instructions (insns delta
0.000%), so the spread is pure code and data placement. **An est_cycles win
below about 0.6% is not measurable with this model.**

**GPU model vs RTL**: 55 distinct demo1 frames captured with `--gpuvec` and
replayed through the os25 `gpu_core` Verilator model gave byte-identical
results to the C model — displayed framebuffer, fence register and a hash of
all 64 MB of SDRAM — and the replayed framebuffer hash equals the `hash_px`
qsim recorded.

**Coverage** of the five workloads: 69,235 distinct executed pcs. Per engine
file (functions with executed code / total, source lines executed):
`r_main.c` 16/16 (91%), `r_bsp.c` 8/8 (98%), `d_polyse.c` 13/15 (73%),
`world.c` 17/17 (95%), `sv_phys.c` 20/21 (73%), `sv_main.c` 18/21 (82%),
`pr_cmds.c` 43/67 (69%), `sbar.c` 18/23 (65%), `menu.c` 22/74 (16%).
53 render functions still execute zero instructions; none of them is reachable
in a default-configured game on the device (the GPU-triangle world path, the
fast-texture tier, multiplayer overlays, debug cvars, paths the port disables
at init, and game events no workload reaches). Regenerate with:

```sh
./qsim baseline.elf --app apps/quake/quake.app --data-dir "$QSIM_DATA_DIR" \
       --coverage --out runs/cov/demo1 --label demo1 --args "+timedemo demo1 +host_framerate 0.1"
./coverage.py --debug-elf g.elf --lines --out runs/cov/report runs/cov/{demo1,demo2,demo3,play1,fx2}
```

`g.elf` is the same build with `-g` (`noise/build_variant.sh g`); coverage.py
checks that its loaded image is byte-identical to the production ELF.

## Gate validation record

The gate's sensitivity was established by running it against deliberately
broken and deliberately harmless builds of the baseline. This is the evidence
behind "lazy + strict is what makes it trustworthy" in the main README.

| ELF | change | gate | caught by |
|---|---|---|---|
| baseline vs baseline | none (50-run determinism check) | **PASS** | — |
| 7 layout variants, `g.elf` | link padding, `-falign-*`, `-g` | **PASS** | — (they must not change rendering) |
| `nosync` | skip the `GPU_FENCE_REACHED` wait in `of_emit_prepare_framebuffer_for_cpu` (one instruction) | **FAIL** | lazy modes only: demo1 `hash_px` 610 frames, `hash_z` 540 |
| `noclean` | `of_emit_cache_clean` returns immediately (one instruction) | **FAIL** | new hazard class `gpu_read_of_uncommitted_cpu_store`, 54,554 events; eager-strict pixel differences |
| `zcached` | `d_pzbuffer` points at the **cached** z alias (one instruction) | **FAIL** | new class `cpu_cached_read_of_stale_line` 17,663 in `D_PolysetDrawSpans8`; eager-strict `hash_px` 264 frames. Eager/flat reported it as a **0.9% win** |
| `uninit` | reads never-initialized memory | **FAIL** | `eager-seed` only: `hash_px` on 957–1120 frames from frame 0 |
| `render_alias` | `D_PolysetDrawSpans8` colormap result +1 | **FAIL** | every workload, every mode (669 demo1 frames, first at frame 36) |
| `render` | `D_DrawParticle` colour +1 | **FAIL** | **`fx2` only** (296 frames): `r_drawparticles` is 0 in every demo |
| `nonrender` | `Con_Printf` computes an unused checksum | **PASS** | — (insns +10,916, est_cycles +0.16% from layout) |
| `datestamp` | the `Exe:` build stamp patched to another date | **PASS** | — (with `--no-normalize-build-stamp` it fails on frames 0–11, as designed) |

All three of `nosync`, `noclean` and `zcached` **passed** an earlier gate that
ran only `eager` + `flat`.

## Findings the gate produced

* **A latent boot race, visible only under `lazy`/`drain`.** demo1–demo3 frame
  2, the top seven console rows: the console background spans reference
  `conback->data` in the Quake cache, `COM_LoadFile` then allocates cache
  memory (`Cache_Alloc`, `memset`), and only *then* does `Draw_BeginDisc`
  (`draw.c:967`) hit the first barrier. If the GPU still holds the spans it
  reads the reallocated bytes. Harmless in practice — it needs the GPU to lag
  behind a file load — but an A/B comparison cannot use that frame, which is
  why the gate has the reference-race rule.
* **The water warp is broken on os25.** `R_RenderView_` puts the 320x200 warp
  buffer on the **stack** (`r_main.c:1127`), the GPU rasterizes the view into
  it (`vid_of.c:321` whitelists `r_warpbuffer` as a GPU destination), and
  `D_WarpScreen` (`d_scan.c:324`) reads it back through the **cached** alias
  after nothing but `of_emit_prepare_framebuffer_for_cpu()`, which only waits
  for the GPU fence (`vid_of.c:842-847`) — there is no
  `of_cache_inval_range` on the warp buffer. With `r_waterwarp 1` on demo2,
  qsim counts 180,455 `cpu_cached_read_of_stale_line` events and 488
  `writeback_reverts_gpu_or_uncached_write`, and in `--mem strict` 185 of the
  1016 frames render differently. The cvar is off by default so no shipped
  configuration hits it, but the missing invalidate is in the source. The
  directed `fx2` workload found this on its first run.
* **A default-off render path hid a real pixel change.** A deliberate
  `D_DrawParticle` colour change passed demo1–demo3 (because
  `r_drawparticles` is 0) and failed `fx2` on 296 frames.
* **Candidates that passed a weaker gate failed the full one.** Three of the
  round-1 candidates passed the three-demo matrix and failed once `play1` and
  `fx2` were added — one with a physics divergence from a frame-time change,
  two with a new `gpu_read_of_uncommitted_cpu_store` hazard class in
  `_gpu_flush_cmd_stream` / `flush_span_batch`. Details in **GATE_R1.md**.
* **The `play1` baseline is clean**: zero memory-coherency hazards in every
  mode, and the only schedule-dependent frames are 2 and 9 (the conback race
  above, and the same effect at the map load).

## Not covered by any workload

Multiplayer/deathmatch code (`Sbar_DeathmatchOverlay`, `Sbar_SortFrags`), the
save/load paths, `changelevel` into the next map, and the CD-audio track
player (the environment exposes no tracks).

## Rebuilding the noise-floor ELFs

`noise/build_variant.sh` builds one functionally identical layout variant of
the baseline in a throwaway git worktree of the Quake port: link padding
(`. += 64/1000/2600` at the start of `.text`, `. += 4096` at the start of
`.data`) or `-falign-functions=32/128` / `-falign-loops=32`. It needs the port
checkout and a build command:

```sh
QUAKE_REPO=~/Repos/Quake QSIM_BUILD_ELF=~/bin/build_quake_elf.sh \
    ./noise/build_variant.sh lt64          # -> noise/elfs/lt64.elf
../../noise_floor.py --app quake --baseline baseline.elf
```

`QSIM_BUILD_ELF <worktree> <objdir>` must leave the ELF at `<objdir>/app.elf`.
The built ELFs are not checked in (they are build artifacts of another repo);
`noise/noise_floor.json` records the measurement they produced.
