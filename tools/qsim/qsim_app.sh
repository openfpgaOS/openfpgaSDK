#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------
#
# qsim_app.sh -- app selection for run.sh and gate.sh (source it).
#
#   qsim_app_select <name|dir>
#
# resolves the app directory (a bare name means apps/<name>), finds its single
# *.app profile, sources its workloads/workloads.sh and exports:
#
#   QSIM_APP            the app name
#   QSIM_APP_DIR        apps/<app>
#   QSIM_APP_PROFILE    apps/<app>/<app>.app          (qsim --app)
#   QSIM_APP_DATA_DIR   host directory with the app's data files (qsim --data-dir),
#                       from QSIM_DATA_DIR / QSIM_PAK_DIR; empty for apps that
#                       declare no data: slots
#
# An app directory contains: <app>.app (+ whatever it references, e.g. an .ini
# template) and workloads/workloads.sh.  See README.md, "Onboarding a new app".
QSIM_SH_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

qsim_app_select() {
    local want="${1:-quake}" dir
    if [ -d "$want" ]; then dir="$(cd "$want" && pwd)"
    elif [ -d "$QSIM_SH_HERE/apps/$want" ]; then dir="$QSIM_SH_HERE/apps/$want"
    else
        echo "qsim: no such app '$want' (known: $(cd "$QSIM_SH_HERE/apps" && echo */ | tr -d /))" >&2
        return 2
    fi
    local profiles=("$dir"/*.app)
    if [ ${#profiles[@]} -ne 1 ] || [ ! -f "${profiles[0]}" ]; then
        echo "qsim: $dir must contain exactly one *.app profile" >&2
        return 2
    fi
    [ -f "$dir/workloads/workloads.sh" ] || {
        echo "qsim: $dir/workloads/workloads.sh is missing" >&2
        return 2
    }
    QSIM_APP="$(basename "$dir")"
    QSIM_APP_DIR="$dir"
    QSIM_APP_PROFILE="${profiles[0]}"
    QSIM_APP_DATA_DIR="${QSIM_DATA_DIR:-${QSIM_PAK_DIR:-}}"
    export QSIM_APP QSIM_APP_DIR QSIM_APP_PROFILE QSIM_APP_DATA_DIR
    # shellcheck source=apps/quake/workloads/workloads.sh
    source "$dir/workloads/workloads.sh"
    # An app that serves data files needs --data-dir; say so once, up front.
    if grep -q '[[:space:]]data:' "$QSIM_APP_PROFILE" && [ -z "$QSIM_APP_DATA_DIR" ]; then
        echo "qsim: app '$QSIM_APP' needs its data files: set QSIM_DATA_DIR=<dir>" >&2
        echo "      (the profile's data: slots, see $QSIM_APP_PROFILE)" >&2
        return 2
    fi
    if [ -n "$QSIM_APP_DATA_DIR" ] && [ ! -d "$QSIM_APP_DATA_DIR" ]; then
        echo "qsim: QSIM_DATA_DIR=$QSIM_APP_DATA_DIR is not a directory" >&2
        return 2
    fi
    return 0
}
