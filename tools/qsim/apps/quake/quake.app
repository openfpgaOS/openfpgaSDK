#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

# qsim app profile -- Quake (openfpgaOS os25 Pocket port, src/quake/engine).
#
#   qsim quake.elf --app apps/quake/quake.app --data-dir <id1> --out <dir> ...
#
# Syntax: see tools/qsim/README.md, "Onboarding a new app".  Everything this
# file sets is app knowledge; the simulator itself knows none of it.

name         = Quake
elf_name     = quake.elf
default_args = +timedemo demo1 +host_framerate 0.1

# Build stamp: the engine prints "Exe: <__TIME__> <__DATE__>" on the console
# (host.c, host_cmd.c), so two builds of the same source made at different
# times would render different pixels in the first console frames.  qsim
# rewrites "<prefix> HH:MM:SS Mon DD YYYY" in the loaded image to a fixed
# date (disable with --no-normalize-build-stamp).
build_stamp  = "Exe: "

# Symbols qsim reads out of the ELF.  frame_symbol is the app's host frame
# counter (frames.csv "host_framecount", used to align A/B runs);
# zbuffer_symbol identifies the depth buffer so hash_z can be taken before
# the frame's z clear.  Both are optional.
frame_symbol   = host_framecount
zbuffer_symbol = zbuffer_storage

# Data-slot registry, in probe order (ENV_SPEC 6.1).  The kernel publishes
# exactly this to the app; the engine then opens files by basename.
#   stub:N        registry entry of N bytes, reads return zeros
#   config:FILE   the app's .ini, $ARGS expanded (also answers config_get)
#   data:FILE     read-only host file under --data-dir (case-insensitive)
#   nv            writable nonvolatile slot, starts empty
slot = 1  os.bin     stub:135400
slot = 2  quake.ini  config:quake.ini.in
slot = 3  quake.elf  stub:876096
slot = 4  pak0.pak   data:pak0.pak
slot = 5  PAK1.PAK   data:pak1.pak
slot = 8  quake.cfg  nv
slot = 10 s0.sav     nv
slot = 11 s1.sav     nv
slot = 12 s2.sav     nv
slot = 13 s3.sav     nv
slot = 14 s4.sav     nv
slot = 15 s5.sav     nv
slot = 16 s6.sav     nv
slot = 17 s7.sav     nv
slot = 18 s8.sav     nv
slot = 19 s9.sav     nv

# Console conventions (POSIX extended regular expressions).
#   result_line  ends the run; capture group 1 is the number of host frames
#                in the measurement window (Quake: the timedemo result).
#   error_line   marks the run as a guest error.
#   parity       facts that must match for two runs to be the same workload;
#                recorded in summary.json ("parity") and compared by gate.sh.
result_line  = ^([0-9]+) frames +[0-9.]+ seconds +[0-9.]+ fps
error_line   = Sys_Error
parity = heap          ([0-9.]+) megabyte heap
parity = surface_cache ([0-9]+)k surface cache
