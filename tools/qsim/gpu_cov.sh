#!/bin/sh
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

# Sum gpu.c corner-case coverage counters over a set of vectors.
cd "$(dirname "$0")"
for v in "$@"; do ./gputest run "$v" /dev/null stats 2>&1; done | \
  awk '/cov /{c[$3]+=$4} /gpu: records/{for(i=2;i<=NF;i++){split($i,kv,"=");gsub(/[()]/,"",kv[1]);if(kv[2]!="")t[kv[1]]+=kv[2]}}
       /gpu: pixels/||/gpu: written/||/gpu: clears/{for(i=2;i<=NF;i++){split($i,kv,"=");t[kv[1]]+=kv[2]}}
       END{for(k in t) printf "total %-22s %d\n",k,t[k]; for(k in c) printf "cov   %-24s %d\n",k,c[k]}' | sort
