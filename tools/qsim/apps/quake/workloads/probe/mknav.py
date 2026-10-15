#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

import math, sys
from mapents import load, vec
def nav_script(m, D, fight_lines, tag):
    ents = load(m)
    st = [e for e in ents if e.get("classname") == "info_player_start"][0]
    so = vec(st["origin"]); ang = float(st.get("angle", "0"))
    dx, dy, dz = D[0]-so[0], D[1]-so[1], D[2]-so[2]
    b = (math.degrees(math.atan2(dy, dx)) - ang + 540) % 360 - 180
    dist = math.hypot(dx, dy)
    L = [f"map {m}", 'alias w5 "wait;wait;wait;wait;wait"', 'alias w10 "w5;w5"', 'alias w50 "w10;w10;w10;w10;w10"',
         "w10;w10", "god", "impulse 9", "w5", "impulse 7", "noclip", "wait"]
    kz = max(0, round((abs(dz) - 10) / 20) + 1) if abs(dz) > 20 else 0
    if kz:
        L += ["+movedown" if dz < 0 else "+moveup"] + ["wait"] * (kz - 1) + ["-movedown" if dz < 0 else "-moveup"]
    if abs(b) > 0.5:
        L += [f"cl_yawspeed {abs(b)*20:.1f}", "+left;wait;-left" if b > 0 else "+right;wait;-right"]
    kf = round((dist - 20) / 32) + 1
    L += ["+forward"] + ["wait"] * (kf - 1) + ["-forward", "wait", "noclip", "w5", "echo QSIM_WINDOW"]
    L += fight_lines
    open(f"{tag}.cfg", "w").write("\n".join(L) + "\n")
    print(tag, m, D, f"bearing {b:.1f} dist {dist:.0f} dz {dz:.0f} kz {kz} kf {kf}")
fight = ["+attack", "impulse 255", "cl_yawspeed 40", "+left", "w50;w50", "impulse 5", "w50;w50", "impulse 255", "impulse 8",
         "w50;w50", "impulse 6", "w50;w50", "impulse 255", "impulse 7", "w50;w50", "-left", "-attack", "+showscores", "w5", "echo QSIM_END"]
nav_script("e2m4", [624.0, 1696.0, 104.0], fight, "n_e2m4a")
nav_script("e2m4", [1288.0, 1376.0, 160.0], fight, "n_e2m4b")
nav_script("e1m6", [352.0, 1296.0, 24.0], fight, "n_e1m6")
nav_script("e3m1", [-416.0, 192.0, -24.0], fight, "n_e3m1")
nav_script("e3m1", [616.0, -1128.0, -56.0], fight, "n_e3m1b")
nav_script("e4m8", [1468.0, 724.0, 160.0], fight, "n_e4m8")
