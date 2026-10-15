#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------
#
# build_variant.sh <variant> -- build one layout-perturbed, functionally identical baseline ELF
# (or the -g coverage-mapping build) in a throwaway git worktree, then remove the worktree.
#
# This is the Quake worked example of how the noise-floor ELFs of ../../../noise_floor.py are
# produced: same source, different code/data placement.  It needs a checkout of the Quake port
# (QUAKE_REPO) and a command that builds its app ELF (QSIM_BUILD_ELF <worktree> <objdir>, which
# must leave the ELF at <objdir>/app.elf); a port with a different build system supplies its own.
#
#   g      CFLAGS/LDFLAGS += -g           (debug info only: .text must equal the baseline; used by coverage.py)
#   lt64   app.ld: . += 64   at the start of .text (every function moves one I-cache line)
#   lt1k   app.ld: . += 1000 at the start of .text
#   lt2k6  app.ld: . += 2600 at the start of .text
#   ld4k   app.ld: . += 4096 at the start of .data (.data/.bss/heap addresses move)
#   af32   CFLAGS += -falign-functions=32 (SDK default 64)
#   al32   CFLAGS += -falign-loops=32     (SDK default 64)
#   af128  CFLAGS += -falign-functions=128
#   <name> apply patches/<name>.patch (validation builds: uninit)
# Output: elfs/<variant>.elf, log in logs/<variant>.log
set -euo pipefail
V="$1"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${QUAKE_REPO:-$HOME/Repos/Quake}"        # the Quake port checkout to build variants of
BUILD="${QSIM_BUILD_ELF:?set QSIM_BUILD_ELF=<script> <worktree> <objdir> -> <objdir>/app.elf}"
SETUP="${QSIM_WT_SETUP:-}"                     # optional: prepare a fresh worktree before building
WT="$HERE/wt-$V"
OBJ="$WT/.obj-qsim"          # inside the worktree: the SDK container mounts only the worktree root
mkdir -p "$HERE/logs" "$HERE/elfs"
exec > "$HERE/logs/$V.log" 2>&1
git -C "$REPO" worktree add --detach "$WT" HEAD
trap 'rc=$?; git -C "$REPO" worktree remove --force "$WT" || true; exit $rc' EXIT
cd "$WT"
[ -n "$SETUP" ] && bash "$SETUP"
MK=src/quake/Makefile LD=src/sdk/app.ld
case "$V" in
    g)     printf 'CFLAGS += -g\nLDFLAGS += -g\n' > /tmp/qsim_mk_$$; sed -i '/^LDFLAGS += -O3 -flto=auto/r /tmp/qsim_mk_'$$ "$MK"; rm /tmp/qsim_mk_$$ ;;
    lt64)  sed -i 's|^        \*(.text.start)$|        *(.text.start)\n        . += 64;|' "$LD" ;;
    lt1k)  sed -i 's|^        \*(.text.start)$|        *(.text.start)\n        . += 1000;|' "$LD" ;;
    lt2k6) sed -i 's|^        \*(.text.start)$|        *(.text.start)\n        . += 2600;|' "$LD" ;;
    ld4k)  sed -i 's|^        \*(.data\*)$|        . += 4096;\n        *(.data*)|' "$LD" ;;
    af32)  sed -i 's|^LDFLAGS += -O3 -flto=auto$|LDFLAGS += -O3 -flto=auto\nCFLAGS += -falign-functions=32\nLDFLAGS += -falign-functions=32|' "$MK" ;;
    al32)  sed -i 's|^LDFLAGS += -O3 -flto=auto$|LDFLAGS += -O3 -flto=auto\nCFLAGS += -falign-loops=32\nLDFLAGS += -falign-loops=32|' "$MK" ;;
    af128) sed -i 's|^LDFLAGS += -O3 -flto=auto$|LDFLAGS += -O3 -flto=auto\nCFLAGS += -falign-functions=128\nLDFLAGS += -falign-functions=128|' "$MK" ;;
    base)  ;;
    *)
        # patches/<name>.patch: any source change (validation builds, e.g. uninit)
        if [ -f "$HERE/patches/$V.patch" ]; then
            git apply "$HERE/patches/$V.patch"
        else
            echo "unknown variant $V (no $HERE/patches/$V.patch)"; exit 2
        fi ;;
esac
git diff | tee "$HERE/logs/$V.diff"
JOBS="${JOBS:-6}" bash "$BUILD" "$WT" "$OBJ"
cp "$OBJ/app.elf" "$HERE/elfs/$V.elf"
sha256sum "$HERE/elfs/$V.elf"
