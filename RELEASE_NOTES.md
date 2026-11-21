# openfpgaOS SDK for Analogue Pocket v0.9.7

This release moves the Pocket SDK to the current kernel and to the os20 and
os25 FPGA images that shipped with Diablo 1.1.3 and Doom 1.2.3. The os30
image and the separate openfpgaOS3D demo core are no longer part of the SDK.

Extract `openfpgaOS-SDK-v0.9.7.zip` to the SD-card root and merge the
folders. Install the whole ZIP: the kernel, the core and the demo ELFs
belong together.

If you installed 0.9.6, delete `Cores/ThinkElastic.openfpgaOS3D`,
`Assets/openfpgaos3d` and `Platforms/openfpgaos3d.json` from the SD card.
That core is not updated by this release.

## Changes

- Audio is louder. The mixer divides the final mix by 4 instead of 16. The
  kernel reports API 0.9.1 so applications can tell which mix-down they run
  on (`of_get_version() >= OF_VERSION(0,9,1)`). Applications that raised
  their own volume to compensate for the old level should lower it.
- The os25 image is the one shipped with Doom 1.2.3: a faster CPU front end
  and a faster GPU memory path. In simulation the CPU test programs take
  5.9% fewer cycles and captured Doom II frames take 35% fewer GPU cycles.
- The os20 image is the one shipped with Diablo 1.1.3: single-cycle shifts,
  per-bank SDRAM open-row tracking and scanout sub-bursts.
- MIDI playback costs less CPU time. In Doom's measurements the cost per
  1 kHz tick falls from 10,719 to 6,646 cycles; the mixer register writes
  are identical to the previous sequencer on the songs tested.
- Fast-texture header fixes in `of_gpu.h` and `of_texture.h`, and optional
  GPU wait hooks (`OF_GPU_WAIT_HOOK`, `OF_GPU_WAIT_PROFILE_*`) that generate
  no code unless an application defines them.
- Removed from `runtime/pocket/`: `os30.rbf_r`, `os30-macxf.rbf_r`,
  `os30-vtx.rbf_r` and `os25zf.rbf_r`. They predate the louder mixer and
  must not be paired with this kernel. `scripts/customize.sh --variant`
  accepts `os20` and `os25`.
- The `triangles` demo is no longer in the Pocket bundle: it needs hardware
  vertex triangles, which neither bundled image has. Its source stays and it
  builds for MiSTer. The bundle contains 22 demo ELFs.

Games that need os30 (Quake2) are unaffected: their releases bundle their own
core and kernel. The MiSTer runtime in the source tree is unchanged.

## Known timing limitation

Both images are configured at 100 MHz and do not close main-clock setup
timing; they are overclocked builds.

| Image | Seed | Setup at 100 MHz | Worst hold |
| --- | ---: | ---: | ---: |
| os20 | 18 | -0.734 ns | +0.071 ns |
| os25 | 22 | -0.431 ns | +0.099 ns |

## Release validation

- All 23 SDK demos build with the containerized RISC-V toolchain; 22 are
  bundled. The desktop builds of `gpudemo`, `mididemo`, `wavdemo` and
  `sdldemo` build.
- All 10 release-packaging regression checks pass.
- Every runtime manifest checksum matches, and the ZIP passes its integrity
  check. The ZIP's kernel, core and loader were compared with the runtime
  files by hash.
- The kernel and the os25 image are the pair released with Doom 1.2.3, which
  was booted and played on a Pocket. The demo bundle itself was not run on a
  Pocket for this release, and the os20 image was run on a Pocket only with
  the Diablo 1.1.3 kernel, which differs from this one in the reported
  version number. If 0.9.7 is less stable than 0.9.6 on your Pocket,
  reinstall 0.9.6 and report it.

## Runtime identity

| Artifact | SHA-256 |
| --- | --- |
| `os.bin` | `103c8c0ac122c8b9a21b1c4f6da3075ed3ec0f1fb20ee30948e0bce65a5d8b57` |
| `os25.rbf_r` | `54a39c1df6fec14a63a9786220d731d6d787d427caf5968f027b83467870e358` |
| `os20.rbf_r` | `71ae8aaf9308fb8296b963086c2a891f3b19bfffbe143075cea08eb65de6cb7f` |
| `loader.bin` | `fc1f8d37eb0fb322006b4428f0b59d2801c9b74978bd3f24103f5f9bbfca3c99` |
