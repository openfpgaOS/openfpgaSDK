#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

# workloads/workloads.sh -- the qsim workload table, shared by run.sh and gate.sh (source it).
#
#   workload_opts <name>   prints the qsim options of workload <name>, one per line
#   workload_names         the known workload names
#   workload_default_set   the workloads gate.sh runs by default
#
# | name   | what                                                                     | ends at            |
# |--------|--------------------------------------------------------------------------|--------------------|
# | demo1  | +timedemo demo1 +host_framerate 0.1 (e1m3 Necropolis; canonical run)     | timedemo result    |
# | demo2  | +timedemo demo2 (e1m4 Grisly Grotto: underwater section)                  | timedemo result    |
# | demo3  | +timedemo demo3 (e1m6 Door to Chthon: sky, lava, mip2)                    | timedemo result    |
# | play1  | live single-player game on e3m1 from workloads/play1.cfg: server physics, | QSIM_END (echo)    |
# |        | QuakeC VM, hull traces, client/server loopback, monsters fighting, UI,     |                    |
# |        | intermission; window = QSIM_WINDOW..QSIM_WINDOW_END (the fight)           |                    |
# | fx2    | directed: demo2 with the non-default render paths on: r_waterwarp 1       | timedemo result    |
# |        | (D_WarpScreen), r_drawparticles 1 (R_DrawParticles/D_DrawParticle),        |                    |
# |        | r_dynamic 1 (R_MarkLights/R_AddDynamicLights), d_mipscale 1.5 (mip3)      |                    |
#
# Device defaults (quake.ini ARGS empty, default.cfg + "gamma 0.7", engine cvar defaults):
# r_waterwarp 0, r_drawparticles 0, r_dynamic 0, d_mipscale 0.3, d_mipcap 0, r_worldtris 0,
# r_hwspan 1, r_gpuworld 1, r_fasttex 1 (falls back to main memory on os25), viewsize 100.
# demo1-3 and play1 use the defaults; fx2 exists only for coverage of the other paths.
WL_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

workload_names() { echo "demo1 demo2 demo3 play1 fx2"; }
workload_default_set() { echo "demo1 demo2 demo3 play1 fx2"; }

workload_opts() {
    case "$1" in
        demo1|demo2|demo3)
            printf '%s\n' --label "$1" --args "+timedemo $1 +host_framerate 0.1" ;;
        play1)
            printf '%s\n' --label play1 --file "play1.cfg=$WL_HERE/play1.cfg" \
                --args "+exec play1.cfg +host_framerate 0.1" \
                --window-on QSIM_WINDOW --window-end-on QSIM_WINDOW_END --end-on QSIM_END ;;
        fx2)
            printf '%s\n' --label fx2 \
                --args "+r_waterwarp 1 +r_drawparticles 1 +r_dynamic 1 +d_mipscale 1.5 +timedemo demo2 +host_framerate 0.1" ;;
        *)
            echo "workloads.sh: unknown workload '$1' (known: $(workload_names))" >&2
            return 1 ;;
    esac
}

# sha256 of everything a workload reads besides the ELF, qsim and the PAKs (for gate.sh stamps)
workload_inputs_sha() {
    case "$1" in
        play1) sha256sum "$WL_HERE/play1.cfg" | cut -d' ' -f1 ;;
        *) echo none ;;
    esac
}
