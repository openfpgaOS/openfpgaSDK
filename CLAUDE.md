# CLAUDE.md — openfpgaOS SDK and game ports

Guidance for AI coding agents (Claude Code reads this file; Codex reads
`AGENTS.md`, which is the same file) working in this SDK or in a game-port
checkout built from it.

## What this is

SDK for **openfpgaOS** — a bare-metal RISC-V game runtime (VexiiRiscv
rv32imafc @ 100 MHz, 64 MB SDRAM) on two platforms: **Analogue Pocket**
and **MiSTer** (DE10-Nano / SuperStation One). One app `.elf` runs
unchanged on both; everything platform-specific arrives at runtime via
the capability/services tables (`of_get_caps()`, auxv). Apps are trusted,
single-process, no MMU — they run close to the hardware.

Game ports (Duke3D, Doom, Quake, QuakeSpasm, Quake2, Diablo, ScummVM, …)
are **forks of this SDK**: same root layout, the game lives in
`src/<name>/` next to SDK-owned `src/sdk/` and `src/apps/`.

## Build, test, deploy

```bash
make build CORE=<name>     # build a custom core (or cd src/<name> && make)
make test  CORE=<name>     # desktop SDL2 build (./app_pc) — fastest iteration
make copy  CORE=<name>     # deploy to Pocket SD card
make copy  CORE=<name> TARGET=mister   # network-push to a MiSTer (MISTER_IP env, default mister.local)
make debug CORE=<name>     # UART push + console stream (Pocket + DevKey only)
```

Do not edit SDK-owned directories: `src/sdk/`, `src/apps/` (except the app
you're working on), `scripts/`, `runtime/` (build artifacts synced from the
openfpgaOS repo), `src/sdk/platforms/mister/fatfs/` (vendored, pristine).

## Porting ground rules

- **Never hardcode addresses or sizes** — read `of_get_caps()` (fb_base,
  heap, sample pool, gpu_base, cpu_freq_hz). The memory map is the
  portability anchor; the caps table is how you stay on it.
- **Gate hardware on feature bits**, not platform ids, wherever possible:
  `of_has_feature(OF_HW_GPU_PERSP)`, `of_has_feature(OF_HW_ANALOGIZER)`, …
  Use `of_get_caps()->platform_id` (`OF_PLATFORM_POCKET/MISTER/SIM`) only
  for genuinely platform-shaped behavior.
- **Pocket-only**: `make debug` (PHDP/UART), `interact.json` menus,
  Analogizer/SNAC. On MiSTer `of_analogizer_enabled()` returns 0.
- Files: register names once (`of_file_slot_register(slot, "game.grp")`),
  then standard `fopen`/`fread`. Filenames ≤ 23 chars (registry limit, both
  platforms). POSIX `lseek` must use the riscv32 5-arg `_llseek` convention
  (see `src/sdk/include/of_posix.c`).
- Keep a CPU fallback for every GPU path and keep the desktop build
  (`make test`) green — it is the fast correctness reference.

### Reference ports (read these before inventing a new pattern)

| Port | GPU | Audio | Input | Key files |
|---|---|---|---|---|
| Duke3D | affine span groups (Build engine) | mixer groups + MIDI | pad + P2 + kbd + mouse | `src/duke3d/d3d_gpu.c`, `d3d_audio.c`, `Engine/src/display_of.c` |
| Doom | affine spans + translucency (fuzz) | mixer channels | pad + analog | `src/doom/cdoom/doom/r_gpu.c`, `shim/i_sdlsound.c`, `shim/i_input.c` |
| Quake | perspective span groups | stream + 2-voice CD music | pad + analog | `src/quake/engine/vid_of.c`, `cd_of.c`, `snd_of.c`, `in_of.c` |
| QuakeSpasm | affine + persp, palookup cache | mixer groups | kbd/mouse shims, deadzone | `src/quake/gpu/qs_gpu.c`, `shim/snd_of.c`, `shim/in_of.c` |
| Quake2 | param span lists (Z modes) | mixer SFX + streamed music | pad | `src/quake2/openfpga/of_emit_q2.c`, `snd_of.c`, `input_of.c` |

Minimal working examples in this repo: `src/apps/gpudemo` (GPU),
`src/apps/wavdemo`/`moddemo`/`mididemo` (audio), `src/apps/sdldemo` (SDL2).

## GPU — optimizing the renderer

The GPU is an **asynchronous indexed-color span rasterizer** built for
BUILD/Doom/Quake-style software renderers. The CPU stages command words in
a cached SDRAM scratch buffer; a doorbell DMA pulls them into the GPU's
16 KB ring; fences report completion. There is no per-command MMIO path —
**batching is the design, not an optimization**.

### Integration pattern

- `of_gpu.h` contains **static mutable state — include it from exactly ONE
  translation unit** and export your own wrapper API to the rest of the
  game (every port does this: `d3d_gpu.c`, `r_gpu.c`, `vid_of.c`, `qs_gpu.c`).
- Init order: `of_get_caps()` → check `OF_HW_GPU_*` bits → `of_gpu_init()`
  → draw a probe span to a scratch framebuffer and verify the pixels →
  fall back to the CPU renderer on failure.
- Gate optional GPU features individually: `OF_HW_GPU_SPAN` is the
  baseline (always set); `OF_HW_GPU_PERSP`, `OF_HW_GPU_PARAM_SPAN_LIST`,
  `OF_HW_GPU_PARAM_SPAN_Z/ZTEST/Q29_SCALE`, `OF_HW_GPU_ALPHA`,
  `OF_HW_GPU_BILINEAR`, `OF_HW_GPU_VCOLOR` are not.

### Which span command to use

- `of_gpu_draw_affine_span_group()` — constant-z texture stepping:
  BUILD walls/floors, Doom columns/spans. Up to 8 lanes per group
  (per-lane fb/tex addr, count, s/t steps, 6-bit light, colormap_id);
  the SDK splits >4 lanes into 4-lane hardware chunks.
- `of_gpu_draw_persp_span_group()` (+ `_batch`) — perspective-correct
  spans (Quake world). Accumulate spans and submit groups; don't emit
  one group per span.
- `of_gpu_draw_param_span_list()` — the unified parametric command
  (affine/persp/solid attr modes, Z write/test, Q29 scale). Quake2's
  renderer sits on this. Gate on `OF_HW_GPU_PARAM_SPAN_LIST`.
- `of_gpu_clear_rect()` / `of_gpu_clear_rect_strided()` — GPU-side clears.
- `of_gpu_submit_command_stream_batch()` — pre-encoded raw streams,
  ≤ 4095 words per batch.

### Colormaps and translucency

- `of_gpu_palookup_upload(slot, data, 16384)` — 16 lighting/colormap
  slots of 16 KB. Upload once (or on level load), reference per-span via
  `colormap_id` + 6-bit `light`. Lighting lookups then cost nothing on
  the CPU.
- `of_gpu_translucency_upload(table, 65536)` — BUILD-style 256×256
  translucency table (GPU decimates to 128×256 internally). Doom's fuzz
  and Duke's translucent sprites use this instead of CPU read-modify-write.

### Synchronization — the rules that prevent corruption

- **The GPU owns the framebuffer.** Render everything through the GPU or
  nothing; mixing CPU writes into a frame the GPU is drawing is the #1
  corruption source.
- Batch a frame's worth of commands, then `of_gpu_fence()` + `of_gpu_kick()`
  (or `of_gpu_submit()`); spin with `of_gpu_fence_reached(token)` /
  `of_gpu_wait(token)`; `of_gpu_finish()` is the blocking drain.
- Page flip from the GPU pipeline: `of_gpu_flip_to(idx)` returns a fence
  token; pair with `of_video_acquire_next(just_flipped_idx, token)` for
  triple buffering without tearing or stalls.
- Before the CPU reads/writes a GPU-rendered framebuffer:
  `of_gpu_prepare_framebuffer_for_cpu()`.
- Before the GPU reads CPU-written memory (textures, span payloads built
  in cached SDRAM): `of_cache_flush_range(ptr, size)`. (`of_gpu_*_upload`
  helpers handle their own coherency.)
- State commands are cached by the SDK (`of_gpu_set_framebuffer`,
  `of_gpu_bind_texture` skip redundant writes) — set them per draw freely.

### GPU don'ts

- Don't kick per span/per column — accumulate and submit per frame or per
  render pass.
- Don't busy-wait on a fence you just emitted if you have CPU work left
  (sound mixing, game logic) — overlap it; the GPU is asynchronous.
- Don't write bulk data through the uncached SDRAM alias "to be safe" —
  it is several times slower than cached writes + `of_cache_flush_range`.
- Don't optimize a renderer that still has bugs. Fix first (pixel-compare
  against the CPU reference renderer), then optimize. This lesson is paid
  for (PocketQuake post-mortem).

## HW mixer — audio

32 hardware voices, 48 kHz output, per-voice resampling (16.16 rate),
volume ramps, loops, pan. Sample memory is plain `malloc` SDRAM —
`of_mixer_alloc_samples()` is deprecated.

### SFX

- `of_mixer_init(32, OF_MIXER_OUTPUT_RATE)` once.
- Use the **handle API + groups**, not raw voice indices:
  `of_mixer_alloc_for_group_h(OF_MIXER_GROUP_SFX, pcm, count, rate, pri, vol)`.
  Handles stay valid across voice stealing — `of_mixer_handle_active(h)`
  tells you whether *your* sound still owns the voice. Groups: `SFX`,
  `MUSIC`, `VOICE`, `AUX` with `of_mixer_set_group_volume()` and a master.
  Allocation scans MUSIC low→high and SFX high→low and steals within the
  same group first, so music never loses a voice to gunfire.
- Completion: `of_mixer_poll_ended_h(handles, max)` batched per frame
  (Duke3D), or `of_mixer_handle_active()` per voice (Doom), or
  `of_mixer_set_end_callback()` (ISR context — keep it tiny).
- Hot paths: `of_mixer_set_voice_h(h, rate, vol_l, vol_r)` updates a
  tracker voice in one call; `of_mixer_set_volume_ramp_h()` for clickless
  fades; `of_mixer_retrigger_h()` to restart without realloc.

### Music — pick one of three patterns

1. **Streaming**: `of_audio_stream_open(rate)` + `of_audio_stream_write()`
   for gapless mono (Quake2 track music), or `of_audio_write()` stereo
   pairs against the `of_audio_free()` ring (16384 pairs).
2. **Looped mixer voices**: two voices, hard-panned L/R, with
   `of_mixer_set_loop_h()` — streamed stereo "CD audio" with zero CPU mixing
   (Quake `cd_of.c`).
3. **MIDI**: `of_midi_init()` + `of_midi_play(buf, len, loop)` with a
   `.ofsf` bank in slot 7; add `$(OF_MIDI_SRC)` to `SRCS`. The pump runs
   on the timer ISR — never call `of_midi_pump()` from the main loop
   while playing.

### Keeping audio alive

`of_file_set_idle_hook(pump_fn)` — the kernel calls your hook while a
blocking file read waits on DMA, so music keeps flowing during SD/disk
access. The hook must never issue a blocking file read itself.

### SDL ports

`SDL_mixer` maps straight onto the HW mixer (`Mix_PlayChannel` →
`of_mixer_play_h`, channel groups → mixer groups) and the audio callback
is auto-pumped from `SDL_PollEvent`/`SDL_Delay`/`SDL_RenderPresent` — no
audio thread. Consequence: **call one of those regularly**; a tight loop
that skips event polling starves audio.

## Input — pads and docked keyboard/mouse

### Pads

- `of_input_poll()` exactly once per frame (`of_input_poll_p0()` if you
  only read player 1), then edge APIs (`of_btn_pressed/released`) or
  `of_input_state(player, &s)` for analog sticks (`joy_lx/ly/rx/ry`,
  ±32767) and triggers.
- **Player 2 is free**: `of_btn_p2()` / `of_input_state(1, &s)` — the
  Pocket dock and MiSTer both deliver a second pad through the same API
  (Duke3D wires it for co-op; most ports just ignore it — don't).
- Analog: set a deadzone (`of_input_set_deadzone(4000)` is the QuakeSpasm
  value; default is 0/raw) and map sticks to look/strafe — d-pad-only
  controls waste docked controllers.

### Docked keyboard and mouse

The Pocket dock reserves Player 3 for keyboard and Player 4 for mouse
reports; the SDK exposes them directly:

```c
of_keyboard_state_t kb; of_input_keyboard_state(&kb);   /* kb.present */
of_mouse_state_t    ms; of_input_mouse_state(&ms);      /* ms.present */
if (of_keyboard_key_pressed(&kb, 0x2C)) jump();          /* HID usage codes */
/* kb.modifiers (OF_KEYMOD_*), kb.keys[8] bitmap + _pressed/_released */
/* ms.dx, ms.dy per-poll deltas; ms.buttons + edges */
```

Best practice (Duke3D `display_of.c`): poll pad + keyboard + mouse every
frame and merge — gamepad is the primary mapping, keyboard/mouse *enhance*
when `present` is set. Always degrade gracefully: handheld (undocked)
players see `present == 0`. MiSTer v1 reports no keyboard/mouse — another
reason the pad mapping must be complete on its own.

### SDL ports

The shim exposes the pad as **both** controller events and synthesized
keyboard events (`of_to_scancode()` in `src/sdk/of_sdl2.c` is the remap
point — tweak per game). Suppress one stream with
`-DOF_SDL_NO_KEYBOARD_EVENTS` / `-DOF_SDL_NO_CONTROLLER_EVENTS` when a
game double-handles input.

## Performance checklist

- [ ] GPU does all framebuffer writes; CPU touches the FB only after
      `of_gpu_prepare_framebuffer_for_cpu()`.
- [ ] GPU commands batched per frame/pass; one kick, fences overlapped
      with CPU work.
- [ ] Colormap/lighting via palookup slots, translucency via the HW table —
      not CPU lookups.
- [ ] Hot inner loops annotated `OF_FASTTEXT` (and tables `OF_FASTDATA`) —
      ~24 KB of zero-wait-state BRAM; spend it on the rasterizer/math
      kernels, not on code that the 128 KB D-cache already serves well.
- [ ] Inner loops in fixed point; the FPU (`OF_HW_FPU`) is real but
      int↔float churn in span loops is not free.
- [ ] Cached writes + `of_cache_flush_range` instead of uncached-alias writes.
- [ ] SFX on mixer voices, not CPU-mixed; music on stream/loop voices/MIDI;
      `of_file_set_idle_hook` keeps audio fed during loads.
- [ ] Frame pacing through flip fences / `of_video_acquire_next`, not
      `usleep` guesswork.

## Working rules for agents

1. **Fix bugs before optimizing.** Verify renderer changes against the CPU
   reference (pixel-exact where the API promises it; `make test` SDL build
   is the cheap harness).
2. Touch only the game's directory in a port repo; SDK files come from
   upstream via `git pull`/`make push`.
3. Keep the public `of_*` API and SDK-visible behavior stable; portability
   between Pocket and MiSTer is a hard requirement (`check-api` gate in
   the OS repo enforces the headers).
4. When adding hardware usage, gate on `of_has_feature()` and keep the
   no-feature fallback working.
5. Platform docs live next to the code: `src/sdk/platforms/mister/README.md`
   (deploy + slot→path contract), the main `README.md` (API reference),
   `GETTING_STARTED.md`. Update them when behavior changes; don't duplicate.
