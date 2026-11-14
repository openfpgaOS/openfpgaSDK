# openfpgaOS SDK for Analogue Pocket v0.9.6

This release updates the Pocket SDK demo bundle from v0.7 to the current
runtime and SDK. Extract `openfpgaOS-SDK-v0.9.6.zip` to the SD-card root and
merge the folders. Install the core and bundled application ELFs together:
the updated os25 GPU command-upload path requires applications built with
the updated SDK.

## Changes

- Refresh the default os25 FPGA image to the retained seed-20 build, keeping
  its CPU/GPU cycle behavior, cache sizes, full hardware mixer, translucency,
  Analogizer and Dock support.
- Rebuild all 23 SDK demo applications against the current SDK, including
  GPU command-upload and soft-reset fence handling, docked keyboard/mouse
  input, and MIDI/sample voice fixes.
- Keep the triangle demo on its separate openfpgaOS3D platform so the Pocket
  loads its os30 image. The os30 image and shared OS/loader retain their
  existing verified pairing.
- Include SDK and musl redistribution notices in the installation file.
  Release packaging now checks archive layout and runtime compatibility.
- The source tree also includes the refreshed os20 radix-4 divider image
  and updated MiSTer application packaging. The Pocket demo ZIP selects
  os25 and os30; MiSTer core distribution remains a separate release.

## Known timing limitation

The refreshed Pocket os20 and os25 images remain configured at 100 MHz but
do not close main-clock setup timing. Their worst-corner estimates are
95.26 MHz (os20, seed 35, setup -0.498 ns) and 96.87 MHz (os25, seed 20,
setup -0.323 ns). These targeted fits pass the recorded functional checks
and used hold/cross-clock checks, but the October 30 refresh was not
validated on Pocket hardware. See `runtime/MANIFEST` and the README's
Pocket bitstream section for provenance and qualification limits.

Os20's mixed CPU/interrupt workload uses 5.5% fewer cycles; this is a
synthetic measurement, not a measured game FPS improvement. No new os25
game FPS gain is claimed.

## Release validation

- All 23 SDK demos build with the containerized RISC-V toolchain.
- All 10 release-packaging regression checks pass.
- Every runtime manifest checksum matches.
- The final ZIP passes its integrity check. Both core metadata versions,
  paired runtime files, all demo ELFs, triangle-platform ELF and component
  notices were checked against the release inputs.

SHA-256 of `openfpgaOS-SDK-v0.9.6.zip`:
`29fbc7dacaf100093936fefecbec82e90289d67261d1d56df1afe6c090cd21a2`.
