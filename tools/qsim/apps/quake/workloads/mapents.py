#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

import os, struct, re, math
# Quake id1 directory (the PAKs).  Same source as qsim --data-dir.
PAK_DIR = os.environ.get("QSIM_DATA_DIR") or os.environ.get("QSIM_PAK_DIR") or "id1"
def load(mapname):
    for p in ["PAK0.PAK","PAK1.PAK"]:
        f=open(os.path.join(PAK_DIR, p),"rb")
        magic,off,ln=struct.unpack("<4sii",f.read(12))
        f.seek(off); d=f.read(ln)
        for i in range(ln//64):
            name,fo,fl=struct.unpack("<56sii",d[i*64:(i+1)*64])
            if name.split(b'\0')[0].decode()==f"maps/{mapname}.bsp":
                f.seek(fo); b=f.read(fl)
                eo,el=struct.unpack("<ii",b[4:12])
                txt=b[eo:eo+el].decode(errors="replace")
                return [dict(re.findall(r'"([^"]*)"\s+"([^"]*)"',blk)) for blk in re.findall(r"\{([^}]*)\}",txt)]
def vec(s): return list(map(float,s.split()))
