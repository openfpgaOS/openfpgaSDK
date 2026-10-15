#!/usr/bin/env python3
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

"""Generate RV32IMAFC instruction stubs for the qsim CPU self-test.

Every stub isolates one "key" instruction (random registers / immediates,
32-bit and compressed encodings).  Outputs:
  stubs.S         guest assembly
  stubs_guest.h   guest table: function pointers + calling kind
  stubs_meta.h    host table: semantics needed to compute expected results
  stubs_keys.txt  "symbol<TAB>expected objdump -M no-aliases text" for the
                  key instruction (checked by build_guests.sh)
Deterministic (fixed seed).
"""
import random, sys, os

R = random.Random(20260915)
out_dir = sys.argv[1] if len(sys.argv) > 1 else "."

IREG = {n: i for i, n in enumerate(
    "zero ra sp gp tp t0 t1 t2 s0 s1 a0 a1 a2 a3 a4 a5 a6 a7 s2 s3 s4 s5 s6 s7 s8 s9 s10 s11 t3 t4 t5 t6".split())}
FREG = ["ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7", "fs0", "fs1", "fa0", "fa1", "fa2", "fa3",
        "fa4", "fa5", "fa6", "fa7", "fs2", "fs3", "fs4", "fs5", "fs6", "fs7", "fs8", "fs9", "fs10",
        "fs11", "ft8", "ft9", "ft10", "ft11"]
# integer regs usable as random operands (t0/t1 are input scratch, ra/sp/gp/tp excluded)
POOL = ["t2", "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "t3", "t4", "t5", "t6"]
POOL3 = ["s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5"]          # x8..x15
FPOOL = [f for f in FREG if f not in ("ft0", "ft1", "ft2", "fs0", "fs1") and not f.startswith("fs")] + ["fs0", "fs1"]
FPOOL3 = ["fs0", "fs1", "fa0", "fa1", "fa2", "fa3", "fa4", "fa5"]

asm = []
guest = []
meta = []
keys = []
n = 0

def pick(pool, zero_ok=False):
    if zero_ok and R.random() < 0.08:
        return "zero"
    return R.choice(pool)

def begin(kind, fl=False):
    global n
    name = "stub%d" % n
    asm.append("    .globl %s\n    .globl %s_key\n    .p2align 1\n%s:" % (name, name, name))
    # save callee-saved s0/s1 (and fs0/fs1 low 32 bits) on the stack
    asm.append("    addi sp, sp, -32\n    sw s0, 0(sp)\n    sw s1, 4(sp)\n    fsw fs0, 8(sp)\n    fsw fs1, 12(sp)")
    asm.append("    mv t0, a0\n    mv t1, a1")
    guest.append("    { %s, %d }," % (name, 1 if fl else 0))
    return name

def end():
    asm.append("    lw s0, 0(sp)\n    lw s1, 4(sp)\n    flw fs0, 8(sp)\n    flw fs1, 12(sp)\n    addi sp, sp, 32\n    ret")

def key(name, text, expect):
    asm.append("%s_key:\n    %s" % (name, text))
    keys.append("%s_key\t%s" % (name, expect))

def add_meta(**kw):
    global n
    d = dict(kind="K_NONE", op=0, rm=0, imm=0, imm2=0, size=0, lsize=0, f=0)
    d.update(kw)
    meta.append("    { %s, %s, %d, %d, %d, %d, %d, 0x%x }," % (
        d["kind"], d["op"], d["rm"], d["imm"], d["imm2"], d["size"], d["lsize"], d["f"]))
    n += 1

# flag bits for meta.f
F_ZA, F_ZB, F_SAME, F_ZD, F_C, F_BACK, F_ALIAS, F_ODD = 1, 2, 4, 8, 16, 32, 64, 128

def rtext(r): return r

# ---------------------------------------------------------------- R-type
ROPS = ["add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra", "or", "and",
        "mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu"]
for op in ROPS:
    for _ in range(6):
        rd, ra, rb = pick(POOL, True), pick(POOL, True), pick(POOL, True)
        name = begin(0)
        f = 0
        if ra != "zero": asm.append("    mv %s, t0" % ra)
        else: f |= F_ZA
        if rb != "zero": asm.append("    mv %s, t1" % rb)
        else: f |= F_ZB
        if ra == rb and ra != "zero": f |= F_SAME
        if rd == "zero": f |= F_ZD
        asm.append("    .option norvc")
        key(name, "%s %s, %s, %s" % (op, rd, ra, rb), "%s\t%s,%s,%s" % (op, rd, ra, rb))
        asm.append("    .option rvc")
        asm.append("    mv a0, %s" % rd)
        end()
        add_meta(kind="K_R", op="OP_" + op.upper(), f=f)

# compressed R-type: c.add c.mv c.sub c.xor c.or c.and
for op in ["add", "mv", "sub", "xor", "or", "and"]:
    for _ in range(10):
        pool = POOL if op in ("add", "mv") else POOL3
        rd = R.choice(pool)
        rb = R.choice(pool)
        name = begin(0)
        f = F_C
        if op == "mv":
            asm.append("    mv %s, t1" % rb)
            key(name, "c.mv %s, %s" % (rd, rb), "c.mv\t%s,%s" % (rd, rb))
            add_meta(kind="K_R", op="OP_MV", f=f)
        else:
            asm.append("    mv %s, t0\n    mv %s, t1" % (rd, rb))
            if rd == rb: f |= F_SAME
            key(name, "c.%s %s, %s" % (op, rd, rb), "c.%s\t%s,%s" % (op, rd, rb))
            add_meta(kind="K_R", op="OP_" + op.upper(), f=f)
        asm.append("    mv a0, %s" % rd)
        end()

# ---------------------------------------------------------------- I-type
def rand_imm12():
    c = R.random()
    if c < 0.3: return R.choice([0, 1, -1, 2047, -2048, 2046, -2047, 16, -16, 255, 256, 0x7ff])
    return R.randint(-2048, 2047)

for op in ["addi", "slti", "sltiu", "xori", "ori", "andi"]:
    for _ in range(12):
        rd, rs = pick(POOL, True), pick(POOL, True)
        imm = rand_imm12()
        name = begin(0)
        f = 0
        if rs != "zero": asm.append("    mv %s, t0" % rs)
        else: f |= F_ZA
        if rd == "zero": f |= F_ZD
        asm.append("    .option norvc")
        key(name, "%s %s, %s, %d" % (op, rd, rs, imm), "%s\t%s,%s,%d" % (op, rd, rs, imm))
        asm.append("    .option rvc")
        asm.append("    mv a0, %s" % rd)
        end()
        add_meta(kind="K_I", op="OP_" + op.upper(), imm=imm, f=f)

for op in ["slli", "srli", "srai"]:
    for sh in list(range(0, 32, 3)) + [31, 1]:
        rd, rs = pick(POOL, True), pick(POOL, True)
        name = begin(0)
        f = 0
        if rs != "zero": asm.append("    mv %s, t0" % rs)
        else: f |= F_ZA
        if rd == "zero": f |= F_ZD
        asm.append("    .option norvc")
        key(name, "%s %s, %s, %d" % (op, rd, rs, sh), "%s\t%s,%s,0x%x" % (op, rd, rs, sh))
        asm.append("    .option rvc")
        asm.append("    mv a0, %s" % rd)
        end()
        add_meta(kind="K_I", op="OP_" + op.upper(), imm=sh, f=f)

# compressed immediates
for _ in range(16):
    rd = R.choice(POOL)
    imm = R.choice([R.randint(-32, 31), -32, 31, 1, -1])
    if imm == 0: imm = 5
    name = begin(0)
    asm.append("    mv %s, t0" % rd)
    key(name, "c.addi %s, %d" % (rd, imm), "c.addi\t%s,%d" % (rd, imm))
    asm.append("    mv a0, %s" % rd)
    end()
    add_meta(kind="K_I", op="OP_ADDI", imm=imm, f=F_C)
for _ in range(10):
    rd = R.choice(POOL)
    imm = R.randint(-32, 31)
    name = begin(0)
    key(name, "c.li %s, %d" % (rd, imm), "c.li\t%s,%d" % (rd, imm))
    asm.append("    mv a0, %s" % rd)
    end()
    add_meta(kind="K_I", op="OP_ADDI", imm=imm, f=F_C | F_ZA)
for _ in range(10):
    rd = R.choice([r for r in POOL if r != "sp"])
    v = R.choice([1, 0x1f, 0xfffe0, 0xfffff, R.randint(1, 31), 0xfffe0 + R.randint(0, 31)])
    name = begin(0)
    key(name, "c.lui %s, 0x%x" % (rd, v), "c.lui\t%s,0x%x" % (rd, v))
    asm.append("    mv a0, %s" % rd)
    end()
    add_meta(kind="K_LUI", imm=(v if v < 0x80000 else v - 0x100000), f=F_C)
for op in ["slli", "srli", "srai"]:
    for _ in range(8):
        sh = R.randint(1, 31)
        rd = R.choice(POOL if op == "slli" else POOL3)
        name = begin(0)
        asm.append("    mv %s, t0" % rd)
        key(name, "c.%s %s, %d" % (op, rd, sh), "c.%s\t%s,0x%x" % (op, rd, sh))
        asm.append("    mv a0, %s" % rd)
        end()
        add_meta(kind="K_I", op="OP_" + op.upper(), imm=sh, f=F_C)
for _ in range(8):
    rd = R.choice(POOL3)
    imm = R.randint(-32, 31)
    name = begin(0)
    asm.append("    mv %s, t0" % rd)
    key(name, "c.andi %s, %d" % (rd, imm), "c.andi\t%s,%d" % (rd, imm))
    asm.append("    mv a0, %s" % rd)
    end()
    add_meta(kind="K_I", op="OP_ANDI", imm=imm, f=F_C)
for _ in range(8):   # c.addi16sp: sp += imm (imm multiple of 16, -512..496, nonzero)
    imm = R.choice([-512, 496, 16, -16] + [16 * R.randint(-32, 31)])
    if imm == 0: imm = 32
    name = begin(0)
    asm.append("    mv t2, sp\n    mv sp, t0")
    key(name, "c.addi16sp sp, %d" % imm, "c.addi16sp\tsp,%d" % imm)
    asm.append("    mv a0, sp\n    mv sp, t2")
    end()
    add_meta(kind="K_I", op="OP_ADDI", imm=imm, f=F_C)
for _ in range(8):   # c.addi4spn rd', sp, uimm (4..1020 multiple of 4)
    rd = R.choice(POOL3)
    imm = R.choice([4, 1020, 4 * R.randint(1, 255)])
    name = begin(0)
    asm.append("    mv t2, sp\n    mv sp, t0")
    key(name, "c.addi4spn %s, sp, %d" % (rd, imm), "c.addi4spn\t%s,sp,%d" % (rd, imm))
    asm.append("    mv sp, t2\n    mv a0, %s" % rd)
    end()
    add_meta(kind="K_I", op="OP_ADDI", imm=imm, f=F_C)

# ---------------------------------------------------------------- LUI / AUIPC
for op in ["lui", "auipc"]:
    for _ in range(10):
        rd = pick(POOL, True)
        v = R.choice([0, 1, 0xfffff, 0x80000, 0x7ffff, R.randint(0, 0xfffff)])
        name = begin(0)
        asm.append("    .option norvc")
        key(name, "%s %s, 0x%x" % (op, rd, v), "%s\t%s,0x%x" % (op, rd, v))
        asm.append("    .option rvc")
        asm.append("    mv a0, %s" % rd)
        end()
        add_meta(kind="K_LUI" if op == "lui" else "K_AUIPC", imm=(v if v < 0x80000 else v - 0x100000),
                 f=F_ZD if rd == "zero" else 0)

# ---------------------------------------------------------------- branches
BOPS = ["beq", "bne", "blt", "bge", "bltu", "bgeu"]
for op in BOPS:
    for pad in [0, 2, 100, 1000, 4070]:
        for back in (0, 1):
            ra, rb = pick(POOL, True), pick(POOL, True)
            name = begin(0)
            f = (F_BACK if back else 0)
            if ra != "zero": asm.append("    mv %s, t0" % ra)
            else: f |= F_ZA
            if rb != "zero": asm.append("    mv %s, t1" % rb)
            else: f |= F_ZB
            if ra == rb and ra != "zero": f |= F_SAME
            asm.append("    .option norvc")
            if not back:
                key(name, "%s %s, %s, %s_t" % (op, ra, rb, name), "%s\t%s,%s," % (op, ra, rb))
                asm.append("    li a0, 0\n    j %s_e" % name)
                if pad: asm.append("    .space %d" % pad)
                asm.append("%s_t:\n    li a0, 1\n%s_e:" % (name, name))
            else:
                asm.append("    j %s_k" % name)
                asm.append("%s_t:\n    li a0, 1\n    j %s_e" % (name, name))
                if pad: asm.append("    .space %d" % pad)
                asm.append("%s_k:" % name)
                key(name, "%s %s, %s, %s_t" % (op, ra, rb, name), "%s\t%s,%s," % (op, ra, rb))
                asm.append("    li a0, 0\n%s_e:" % name)
            asm.append("    .option rvc")
            end()
            add_meta(kind="K_BR", op="OP_" + op.upper(), f=f)
for op in ["beqz", "bnez"]:
    for pad in [0, 2, 60, 200]:
        for back in (0, 1):
            ra = R.choice(POOL3)
            name = begin(0)
            f = F_C | F_ZB | (F_BACK if back else 0)
            asm.append("    mv %s, t0" % ra)
            if not back:
                key(name, "c.%s %s, %s_t" % (op, ra, name), "c.%s\t%s," % (op, ra))
                asm.append("    li a0, 0\n    j %s_e" % name)
                if pad: asm.append("    .space %d" % pad)
                asm.append("%s_t:\n    li a0, 1\n%s_e:" % (name, name))
            else:
                asm.append("    j %s_k" % name)
                asm.append("%s_t:\n    li a0, 1\n    j %s_e" % (name, name))
                if pad: asm.append("    .space %d" % pad)
                asm.append("%s_k:" % name)
                key(name, "c.%s %s, %s_t" % (op, ra, name), "c.%s\t%s," % (op, ra))
                asm.append("    li a0, 0\n%s_e:" % name)
            end()
            add_meta(kind="K_BR", op="OP_BEQ" if op == "beqz" else "OP_BNE", f=f)

# ---------------------------------------------------------------- jumps
# result = link value (or marker 1 for no-link jumps); host knows <name>_key
for form in ["jal", "jal0", "c.j", "c.jal", "jalr", "jalr0", "c.jr", "c.jalr", "jalr_odd"]:
    for pad in [0, 40, 1500]:
        rd = R.choice([r for r in POOL if r not in ("t2", "t3", "t4")])
        rs = R.choice([r for r in POOL if r != "t2"])
        name = begin(0)
        asm.append("    mv t2, ra")
        f = 0
        if form in ("jal", "jal0"):
            link = rd if form == "jal" else "zero"
            asm.append("    li %s, 0x55\n    .option norvc" % (rd))
            key(name, "jal %s, %s_t" % (link, name), "jal\t%s," % link)
            asm.append("    .option rvc")
            if link == "zero": f |= F_ZD
            res = rd if form == "jal" else None
        elif form in ("c.j", "c.jal"):
            if form == "c.jal":
                key(name, "c.jal %s_t" % name, "c.jal\t")
                res = "ra"
            else:
                key(name, "c.j %s_t" % name, "c.j\t")
                res = None
                f |= F_ZD
            f |= F_C
        else:
            asm.append("    .option norvc\n    la t3, %s_t\n    .option rvc" % name)
            imm = R.choice([0, 4, -4, 2047, -2048, R.randint(-2048, 2047)])
            odd = 1 if form == "jalr_odd" else 0
            if form in ("c.jr", "c.jalr"):
                imm = 0
            asm.append("    li t4, %d\n    sub %s, t3, t4" % (imm - odd, rs))
            if form in ("jalr", "jalr0", "jalr_odd"):
                link = rd if form != "jalr0" else "zero"
                if form == "jalr_odd": f |= F_ODD
                asm.append("    li %s, 0x55\n    .option norvc" % rd if (link != "zero" and rd != rs) else "    .option norvc")
                key(name, "jalr %s, %d(%s)" % (link, imm, rs), "jalr\t%s,%d(%s)" % (link, imm, rs))
                asm.append("    .option rvc")
                res = link if link != "zero" else None
                if link == "zero": f |= F_ZD
            elif form == "c.jr":
                key(name, "c.jr %s" % rs, "c.jr\t%s" % rs)
                res = None
                f |= F_C | F_ZD
            else:
                key(name, "c.jalr %s" % rs, "c.jalr\t%s" % rs)
                res = "ra"
                f |= F_C
        asm.append("    li a0, 0x77\n    j %s_e" % name)
        if pad: asm.append("    .space %d" % pad)
        asm.append("    .p2align 1\n%s_t:" % name)
        if res: asm.append("    mv a0, %s" % res)
        else: asm.append("    li a0, 1")
        asm.append("%s_e:\n    mv ra, t2" % name)
        end()
        add_meta(kind="K_JUMP", f=f)

# ---------------------------------------------------------------- loads / stores
LOADS = [("lb", 1, 1), ("lbu", 1, 0), ("lh", 2, 1), ("lhu", 2, 0), ("lw", 4, 0)]
STORES = [("sb", 1), ("sh", 2), ("sw", 4)]
for sop, ss in STORES:
    for lop, ls, sgn in LOADS:
        if ls > ss: continue
        for _ in range(4):
            MP = [r for r in POOL if r not in ("t4", "t5")]
            rb, rv, rd = R.choice(MP), R.choice(MP), R.choice(POOL)
            while rv == rb: rv = R.choice(MP)
            imm = R.randint(-2000, 2000)
            imm2 = imm + R.randint(0, ss - ls)
            alias = R.random() < 0.3
            name = begin(0)
            asm.append("    mv %s, t0\n    mv %s, t1" % (rb, rv))
            asm.append("    .option norvc")
            key(name, "%s %s, %d(%s)" % (sop, rv, imm, rb), "%s\t%s,%d(%s)" % (sop, rv, imm, rb))
            rl = rb
            if alias:
                asm.append("    li t4, 0x40000000\n    add t5, %s, t4" % rb)
                rl = "t5"
            asm.append("    %s %s, %d(%s)" % (lop, rd, imm2, rl))
            asm.append("    .option rvc\n    mv a0, %s" % rd)
            end()
            add_meta(kind="K_MEM", op="OP_" + lop.upper(), imm=imm, imm2=imm2, size=ss, lsize=ls,
                     f=F_ALIAS if alias else 0)
for _ in range(10):   # c.sw / c.lw
    rb, rv, rd = R.choice(POOL3), R.choice(POOL3), R.choice(POOL3)
    while rv == rb: rv = R.choice(POOL3)
    imm = 4 * R.randint(0, 31)
    name = begin(0)
    asm.append("    mv %s, t0\n    mv %s, t1" % (rb, rv))
    key(name, "c.sw %s, %d(%s)" % (rv, imm, rb), "c.sw\t%s,%d(%s)" % (rv, imm, rb))
    asm.append("    c.lw %s, %d(%s)\n    mv a0, %s" % (rd, imm, rb, rd))
    end()
    add_meta(kind="K_MEM", op="OP_LW", imm=imm, imm2=imm, size=4, lsize=4, f=F_C)
for _ in range(10):   # c.swsp / c.lwsp
    rv, rd = R.choice(POOL), R.choice(POOL)
    imm = 4 * R.randint(0, 63)
    name = begin(0)
    asm.append("    mv t2, sp\n    mv sp, t0\n    mv %s, t1" % rv)
    key(name, "c.swsp %s, %d(sp)" % (rv, imm), "c.swsp\t%s,%d(sp)" % (rv, imm))
    asm.append("    c.lwsp %s, %d(sp)\n    mv sp, t2\n    mv a0, %s" % (rd, imm, rd))
    end()
    add_meta(kind="K_MEM", op="OP_LW", imm=imm, imm2=imm, size=4, lsize=4, f=F_C)
# float store/load: fsw/flw, c.fsw/c.flw, c.fswsp/c.flwsp
for form in ["32", "c", "csp"]:
    for _ in range(6):
        fa, fb = R.choice(FPOOL), R.choice(FPOOL)
        name = begin(0)
        if form == "32":
            rb = R.choice(POOL)
            imm = R.randint(-2000, 2000)
            asm.append("    mv %s, t0\n    fmv.w.x %s, t1\n    .option norvc" % (rb, fa))
            key(name, "fsw %s, %d(%s)" % (fa, imm, rb), "fsw\t%s,%d(%s)" % (fa, imm, rb))
            asm.append("    flw %s, %d(%s)\n    .option rvc" % (fb, imm, rb))
        elif form == "c":
            rb = R.choice(POOL3)
            fa, fb = R.choice(FPOOL3), R.choice(FPOOL3)
            imm = 4 * R.randint(0, 31)
            asm.append("    mv %s, t0\n    fmv.w.x %s, t1" % (rb, fa))
            key(name, "c.fsw %s, %d(%s)" % (fa, imm, rb), "c.fsw\t%s,%d(%s)" % (fa, imm, rb))
            asm.append("    c.flw %s, %d(%s)" % (fb, imm, rb))
        else:
            imm = 4 * R.randint(0, 63)
            asm.append("    mv t2, sp\n    mv sp, t0\n    fmv.w.x %s, t1" % fa)
            key(name, "c.fswsp %s, %d(sp)" % (fa, imm), "c.fswsp\t%s,%d(sp)" % (fa, imm))
            asm.append("    c.flwsp %s, %d(sp)\n    mv sp, t2" % (fb, imm))
        asm.append("    fmv.x.w a0, %s" % fb)
        end()
        add_meta(kind="K_FMEM", imm=imm, size=4, f=F_C if form != "32" else 0)

# ---------------------------------------------------------------- A extension
for op in ["amoswap", "amoadd", "amoxor", "amoand", "amoor", "amomin", "amomax", "amominu", "amomaxu", "lrsc"]:
    for _ in range(4):
        AP = ["s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "t5"]
        rb, rv, rd = R.choice(AP), R.choice(AP), R.choice(AP)
        while rv == rb: rv = R.choice(AP)
        while rd in (rb, rv): rd = R.choice(AP)
        name = begin(0)
        asm.append("    mv %s, t0\n    mv %s, t1\n    not t2, t1\n    sw t2, 0(%s)" % (rb, rv, rb))
        asm.append("    .option norvc")
        if op == "lrsc":
            key(name, "lr.w %s, (%s)" % (rd, rb), "lr.w\t%s,(%s)" % (rd, rb))
            asm.append("    sc.w t6, %s, (%s)" % (rv, rb))
            asm.append("    .option rvc\n    lw t3, 0(%s)\n    slli t4, t3, 7\n    srli t3, t3, 25\n    or t3, t3, t4"
                       "\n    xor a0, %s, t3\n    slli t6, t6, 31\n    xor a0, a0, t6" % (rb, rd))
        else:
            key(name, "%s.w %s, %s, (%s)" % (op, rd, rv, rb), "%s.w\t%s,%s,(%s)" % (op, rd, rv, rb))
            asm.append("    .option rvc\n    lw t3, 0(%s)\n    slli t4, t3, 7\n    srli t3, t3, 25\n    or t3, t3, t4"
                       "\n    xor a0, %s, t3" % (rb, rd))
        end()
        add_meta(kind="K_AMO", op="OP_" + op.upper())

# ---------------------------------------------------------------- CSR
CSRS = [("mscratch", 0x340, 0xffffffff), ("fflags", 0x001, 0x1f), ("frm", 0x002, 0x7), ("fcsr", 0x003, 0xff),
        ("mtvec", 0x305, 0xfffffffd), ("mepc", 0x341, 0xfffffffe)]
for cname, cnum, mask in CSRS:
    for op in ["csrrw", "csrrs", "csrrc", "csrrwi", "csrrsi", "csrrci"]:
        CP = [r for r in POOL if r not in ("t3", "t4", "t6")]
        rd = pick(CP, True)
        rs = R.choice(CP)
        uimm = R.choice([0, 1, 31, R.randint(0, 31)])
        name = begin(0)
        asm.append("    csrr t6, mtvec\n    csrw %s, t0" % cname)   # a0 -> csr (a1 = operand)
        imm_form = op.endswith("i")
        if not imm_form:
            asm.append("    mv %s, t1" % rs)
            key(name, "%s %s, %s, %s" % (op, rd, cname, rs), "%s\t%s,%s,%s" % (op, rd, cname, rs))
        else:
            key(name, "%s %s, %s, %d" % (op, rd, cname, uimm), "%s\t%s,%s,%d" % (op, rd, cname, uimm))
        asm.append("    csrr t3, %s\n    slli t4, t3, 13\n    srli t3, t3, 19\n    or t3, t3, t4\n    xor a0, %s, t3"
                   % (cname, rd))
        asm.append("    csrw mtvec, t6\n    csrw fcsr, zero")
        end()
        add_meta(kind="K_CSR", op="OP_" + op.upper(), imm=cnum, imm2=uimm, f=F_ZD if rd == "zero" else 0)

# ---------------------------------------------------------------- misc no-ops
for text, expect in [("fence", None), ("wfi", "wfi"), ("c.nop", None), ("nop", None),
                     (".insn i 0x0F, 2, x0, 0(a5)", None), (".word 0x0027a00f", None), (".word 0x0017a00f", None),
                     (".word 0x0000100f", None)]:
    name = begin(0)
    asm.append("    mv a5, t0\n    li a0, 0x1234\n    .option %s" % ("rvc" if text.startswith("c.") else "norvc"))
    key(name, text, expect or "-")
    asm.append("    .option rvc")
    end()
    add_meta(kind="K_CONST", imm=0x1234)

# illegal instructions (guest trap handler returns 0xdead0000 | mcause in a0)
FRM5 = "0x%08x" % ((11 << 20) | (10 << 15) | (5 << 12) | (10 << 7) | 0x53)
FRM6 = "0x%08x" % ((0x0c << 25) | (11 << 20) | (10 << 15) | (6 << 12) | (10 << 7) | 0x53)
FMA5 = "0x%08x" % ((12 << 27) | (11 << 20) | (10 << 15) | (5 << 12) | (10 << 7) | 0x43)
SHORT = ["0x0000", "0x4002", "0x8002", "0x0004", "0x9001", "0x6101", "0x9c01"]
for w in ["0x00000000", "0x7fffffff", "0x02b5d553", FRM5, FRM6, FMA5, "0xc0305553", "0x00001073", "0x0045a00f",
          "0x00100033 | (0x40 << 25)", "0x0200405b", "0x62a5f543", "0x0000c073", "0x00200073", "0x30200073 | (1 << 15)",
          "0x0000402f", "0x2800202f"] + SHORT:
    name = begin(0)
    if w in SHORT:
        asm.append("%s_key:\n    .2byte %s" % (name, w))
    else:
        asm.append("%s_key:\n    .word %s" % (name, w))
    keys.append("%s_key\t-" % name)
    end()
    add_meta(kind="K_ILLEGAL")

# ---------------------------------------------------------------- F extension
RMS = [("rne", 0), ("rtz", 1), ("rdn", 2), ("rup", 3), ("rmm", 4), ("dyn", 7)]
RMNAME = {0: "rne", 1: "rtz", 2: "rdn", 3: "rup", 4: "rmm", 7: "dyn"}
def fstub(opid, text_fn, nin, out_int, in_int=False, rms=None, count=1):
    for rmname, rm in (rms or [(None, 0)]):
        for _ in range(count):
            fa, fb, fc, fd = R.choice(FPOOL), R.choice(FPOOL), R.choice(FPOOL), R.choice(FPOOL)
            ra, rd = R.choice(POOL), pick(POOL, True)
            name = begin(0, True)
            f = 0
            # inputs: a0 (int), fa0..fa2 (float) -> copy through ft0..ft2 first
            asm.append("    fmv.x.w t3, fa0\n    fmv.x.w t4, fa1\n    fmv.x.w t5, fa2")
            asm.append("    fmv.w.x ft0, t3\n    fmv.w.x ft1, t4\n    fmv.w.x ft2, t5")
            if in_int: asm.append("    mv %s, t0" % ra)
            else:
                asm.append("    fsgnj.s %s, ft0, ft0" % fa)
                if nin > 1: asm.append("    fsgnj.s %s, ft1, ft1" % fb)
                if nin > 2: asm.append("    fsgnj.s %s, ft2, ft2" % fc)
                if nin > 2 and fa == fc: f |= 0x800
                elif nin > 1 and fa == fb: f |= 0x400
                if nin > 2 and fb == fc: f |= 0x1000
            asm.append("    .option norvc")
            txt, exp = text_fn(fd if not out_int else rd, fa if not in_int else ra, fb, fc, rmname)
            key(name, txt, exp)
            asm.append("    .option rvc")
            if out_int:
                asm.append("    mv a0, %s" % rd)
                if rd == "zero": f |= F_ZD
            else:
                asm.append("    fmv.x.w a0, %s" % fd)
            end()
            add_meta(kind="K_F", op="ROP_" + opid, rm=rm, f=f)

def rm_suffix(rm):
    return "" if rm is None else ", %s" % rm
def exp_rm(rm):
    # objdump prints the rounding mode unless it is dyn
    return "" if rm in (None, "dyn") else ",%s" % rm

for opn, mn in [("ADD", "fadd.s"), ("SUB", "fsub.s"), ("MUL", "fmul.s"), ("DIV", "fdiv.s")]:
    fstub(opn, lambda d, a, b, c, rm, mn=mn: ("%s %s, %s, %s%s" % (mn, d, a, b, rm_suffix(rm)),
                                              "%s\t%s,%s,%s%s" % (mn, d, a, b, exp_rm(rm))), 2, False, rms=RMS, count=2)
fstub("SQRT", lambda d, a, b, c, rm: ("fsqrt.s %s, %s%s" % (d, a, rm_suffix(rm)), "fsqrt.s\t%s,%s%s" % (d, a, exp_rm(rm))),
      1, False, rms=RMS, count=2)
for opn, mn in [("MIN", "fmin.s"), ("MAX", "fmax.s"), ("SGNJ", "fsgnj.s"), ("SGNJN", "fsgnjn.s"), ("SGNJX", "fsgnjx.s")]:
    fstub(opn, lambda d, a, b, c, rm, mn=mn: ("%s %s, %s, %s" % (mn, d, a, b), "%s\t%s,%s,%s" % (mn, d, a, b)), 2, False, count=4)
for opn, mn in [("EQ", "feq.s"), ("LT", "flt.s"), ("LE", "fle.s")]:
    fstub(opn, lambda d, a, b, c, rm, mn=mn: ("%s %s, %s, %s" % (mn, d, a, b), "%s\t%s,%s,%s" % (mn, d, a, b)), 2, True, count=4)
fstub("CLASS", lambda d, a, b, c, rm: ("fclass.s %s, %s" % (d, a), "fclass.s\t%s,%s" % (d, a)), 1, True, count=4)
fstub("MV_XW", lambda d, a, b, c, rm: ("fmv.x.w %s, %s" % (d, a), "fmv.x.w\t%s,%s" % (d, a)), 1, True, count=4)
fstub("MV_WX", lambda d, a, b, c, rm: ("fmv.w.x %s, %s" % (d, a), "fmv.w.x\t%s,%s" % (d, a)), 1, False, in_int=True, count=4)
fstub("CVT_W", lambda d, a, b, c, rm: ("fcvt.w.s %s, %s%s" % (d, a, rm_suffix(rm)), "fcvt.w.s\t%s,%s%s" % (d, a, exp_rm(rm))),
      1, True, rms=RMS, count=2)
fstub("CVT_WU", lambda d, a, b, c, rm: ("fcvt.wu.s %s, %s%s" % (d, a, rm_suffix(rm)), "fcvt.wu.s\t%s,%s%s" % (d, a, exp_rm(rm))),
      1, True, rms=RMS, count=2)
fstub("CVT_SW", lambda d, a, b, c, rm: ("fcvt.s.w %s, %s%s" % (d, a, rm_suffix(rm)), "fcvt.s.w\t%s,%s%s" % (d, a, exp_rm(rm))),
      1, False, in_int=True, rms=RMS, count=2)
fstub("CVT_SWU", lambda d, a, b, c, rm: ("fcvt.s.wu %s, %s%s" % (d, a, rm_suffix(rm)), "fcvt.s.wu\t%s,%s%s" % (d, a, exp_rm(rm))),
      1, False, in_int=True, rms=RMS, count=2)
for opn, mn in [("MADD", "fmadd.s"), ("MSUB", "fmsub.s"), ("NMSUB", "fnmsub.s"), ("NMADD", "fnmadd.s")]:
    fstub(opn, lambda d, a, b, c, rm, mn=mn: ("%s %s, %s, %s, %s%s" % (mn, d, a, b, c, rm_suffix(rm)),
                                              "%s\t%s,%s,%s,%s%s" % (mn, d, a, b, c, exp_rm(rm))), 3, False, rms=RMS, count=2)

# ---------------------------------------------------------------- write files
with open(os.path.join(out_dir, "stubs.S"), "w") as fo:
    fo.write("/* generated by gen_stubs.py -- do not edit */\n    .section .text.stubs,\"ax\"\n"
             "    .option rvc\n    .option norelax\n")
    fo.write("\n".join(asm) + "\n")
with open(os.path.join(out_dir, "stubs_guest.h"), "w") as fo:
    fo.write("/* generated */\n#define NSTUBS %d\n" % n)
    fo.write("".join("extern void stub%d(void);\n" % i for i in range(n)))
    fo.write("static const struct { void (*fn)(void); int fl; } stubs[NSTUBS] = {\n")
    fo.write("\n".join(guest) + "\n};\n")
with open(os.path.join(out_dir, "stubs_meta.h"), "w") as fo:
    fo.write("/* generated */\n#define NSTUBS %d\n" % n)
    fo.write("static const stub_meta stub_tab[NSTUBS] = {\n" + "\n".join(meta) + "\n};\n")
with open(os.path.join(out_dir, "stubs_keys.txt"), "w") as fo:
    fo.write("\n".join(keys) + "\n")
print("generated %d stubs" % n)
