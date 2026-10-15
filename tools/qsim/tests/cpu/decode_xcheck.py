#!/usr/bin/env python3
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

"""Cross-check qsim's instruction decoder against GNU objdump.

  decode_xcheck.py gen  OUTDIR        write words.S / words.bin / csrs.S
  decode_xcheck.py cmp  OUTDIR        compare words.dis (objdump -d -M no-aliases of
                                      words.o assembled with the os25 -march) and
                                      csrs.dis against decode_dump output qsim.txt

Words: every 16-bit pattern whose low bits are not 0b11 (49152), plus random
32-bit words biased toward the RV32IMAFC major opcodes.  For each word the
decoded operation, register fields and immediate must match objdump's
disassembly; instructions objdump knows but qsim rejects (or vice versa) are
mismatches unless listed in KNOWN (documented binutils/core differences).
"""
import os, re, struct, sys, random

BASE = 0x10000000
mode, out = sys.argv[1], sys.argv[2]

IREG = "zero ra sp gp tp t0 t1 t2 s0 s1 a0 a1 a2 a3 a4 a5 a6 a7 s2 s3 s4 s5 s6 s7 s8 s9 s10 s11 t3 t4 t5 t6".split()
FREG = "ft0 ft1 ft2 ft3 ft4 ft5 ft6 ft7 fs0 fs1 fa0 fa1 fa2 fa3 fa4 fa5 fa6 fa7 fs2 fs3 fs4 fs5 fs6 fs7 fs8 fs9 fs10 fs11 ft8 ft9 ft10 ft11".split()
RI = {n: i for i, n in enumerate(IREG)}
RF = {n: i for i, n in enumerate(FREG)}
RM = {"rne": 0, "rtz": 1, "rdn": 2, "rup": 3, "rmm": 4, "dyn": 7}

if mode == "gen":
    R = random.Random(12345)
    words = []
    for w in range(0x10000):
        if (w & 3) != 3:
            words.append((w, 2))
    majors = [0x03, 0x07, 0x0f, 0x13, 0x17, 0x23, 0x27, 0x2f, 0x33, 0x37, 0x43, 0x47, 0x4b, 0x4f,
              0x53, 0x63, 0x67, 0x6f, 0x73]
    for i in range(250000):
        r = R.getrandbits(32)
        if i % 4:
            r = (r & ~0x7f) | R.choice(majors)
            if (r & 0x7f) == 0x53 and R.random() < 0.7:          # focus funct7 on single precision
                r = (r & 0x01ffffff) | (R.choice([0x00, 0x04, 0x08, 0x0c, 0x2c, 0x10, 0x14, 0x60, 0x68, 0x70,
                                                   0x78, 0x50, 0x01, 0x20, 0x58]) << 25)
            if (r & 0x7f) == 0x33 and R.random() < 0.7:
                r = (r & 0x01ffffff) | (R.choice([0, 1, 0x20]) << 25)
            if (r & 0x7f) == 0x73 and R.random() < 0.3:
                r = R.choice([0x73, 0x100073, 0x30200073, 0x10500073, 0x10200073])
            if (r & 0x7f) == 0x0f and R.random() < 0.5:
                r = (r & 0x000f8000) | (R.choice([0, 1, 2, 4, 3]) << 20) | 0x200f
        if (r & 3) != 3:
            r |= 3
        if (r & 0x1f) == 0x1f:
            r &= ~0x04
        words.append((r, 4))
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "words.bin"), "wb") as fb, open(os.path.join(out, "words.S"), "w") as fs:
        fs.write(".text\n")
        for w, n in words:
            if n == 2:
                # 4-byte slots: objdump may swallow 4 bytes for an unknown 16-bit word
                fb.write(struct.pack("<HH", w, 1)); fs.write(".insn 0x%04x\n.insn 0x0001\n" % w)
            else:
                fb.write(struct.pack("<I", w)); fs.write(".insn 0x%08x\n" % w)
    with open(os.path.join(out, "csrs.S"), "w") as fs:
        fs.write(".text\n.option norvc\n")
        for c in range(4096):
            fs.write(".insn 0x%08x\n" % ((c << 20) | (11 << 15) | (1 << 12) | (10 << 7) | 0x73))
    print("words: %d" % len(words))
    sys.exit(0)

# ------------------------------------------------------------------ compare
dis_name, qsim_name, slot_check, csr_dir = "words.dis", "qsim.txt", True, out
if mode == "elf":
    # decode_xcheck.py elf ELFDIR XCHECKDIR app.elf decode_dump
    #   ELFDIR holds app_noalias.dis (objdump -d -M no-aliases of the ELF)
    import subprocess
    from elftools.elf.elffile import ELFFile
    csr_dir, elf_path, dumper = sys.argv[3], sys.argv[4], sys.argv[5]
    BASE, slot_check, dis_name, qsim_name = 0, False, "app_noalias.dis", "qsim_elf.txt"
    funcs = []
    with open(elf_path, "rb") as f:
        ef = ELFFile(f)
        for s in ef.get_section_by_name(".symtab").iter_symbols():
            if s["st_info"]["type"] == "STT_FUNC" and s["st_size"] > 0:
                funcs.append((s["st_value"], s["st_value"] + s["st_size"]))
    funcs.sort()
    import bisect
    starts = [a for a, b in funcs]
    def in_func(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return i >= 0 and addr < funcs[i][1]
    lines = []
    for line in open(os.path.join(out, dis_name)):
        m = re.match(r"^\s*([0-9a-f]+):\s+([0-9a-f]+)\s+(\S+)", line)
        if m and in_func(int(m.group(1), 16)):
            lines.append("%s %s\n" % (m.group(1), m.group(2)))
    r = subprocess.run([dumper, "-"], input="".join(lines), capture_output=True, text=True, check=True)
    open(os.path.join(out, qsim_name), "w").write(r.stdout)

def objdump_lines(path):
    res = {}
    for line in open(path):
        m = re.match(r"^\s*([0-9a-f]+):\s+([0-9a-f]+)\s+(\S+)\s*(.*)$", line)
        if m:
            res[int(m.group(1), 16)] = (int(m.group(2), 16), m.group(3), m.group(4).split("#")[0].strip())
    return res

csr_num = {}
for addr, (w, mn, ops) in objdump_lines(os.path.join(csr_dir, "csrs.dis")).items():
    if mn == "csrrw":
        name = ops.split(",")[1]
        csr_num[name] = (w >> 20) & 0xfff

def num(tok):
    tok = tok.strip()
    return int(tok, 0)

def reg(tok):
    tok = tok.strip()
    if tok in RI: return ("i", RI[tok])
    if tok in RF: return ("f", RF[tok])
    raise ValueError("reg " + tok)

def memop(tok):
    m = re.match(r"^(-?(?:0x)?[0-9a-f]*)\((\w+)\)$", tok.strip())
    if not m: raise ValueError("mem " + tok)
    return (int(m.group(1), 0) if m.group(1) else 0), reg(m.group(2))[1]

def target(tok, pc):
    m = re.match(r"^([0-9a-f]+)", tok.strip())
    return (int(m.group(1), 16) - pc) & 0xffffffff

def s32(v):
    v &= 0xffffffff
    return v - (1 << 32) if v & 0x80000000 else v

def canon(mn, ops, pc, w):
    """objdump -> (op, rd, rs1, rs2, imm) in qsim's field conventions"""
    o = [x.strip() for x in re.split(r",(?![^(]*\))", ops)] if ops else []
    cmn = mn[2:] if mn.startswith("c.") else mn
    R3 = {"add": "ADD", "sub": "SUB", "sll": "SLL", "slt": "SLT", "sltu": "SLTU", "xor": "XOR", "srl": "SRL",
          "sra": "SRA", "or": "OR", "and": "AND", "mul": "MUL", "mulh": "MULH", "mulhsu": "MULHSU",
          "mulhu": "MULHU", "div": "DIV", "divu": "DIVU", "rem": "REM", "remu": "REMU"}
    I3 = {"addi": "ADDI", "slti": "SLTI", "sltiu": "SLTIU", "xori": "XORI", "ori": "ORI", "andi": "ANDI",
          "slli": "SLLI", "srli": "SRLI", "srai": "SRAI"}
    if mn in R3:
        return (R3[mn], reg(o[0])[1], reg(o[1])[1], reg(o[2])[1], None)
    if mn in I3:
        return (I3[mn], reg(o[0])[1], reg(o[1])[1], None, s32(num(o[2])))
    if mn.startswith("c."):
        if cmn in ("add", "sub", "xor", "or", "and"):
            return (R3[cmn], reg(o[0])[1], reg(o[0])[1], reg(o[1])[1], None)
        if cmn == "mv": return ("ADD", reg(o[0])[1], 0, reg(o[1])[1], None)
        if cmn == "addi": return ("ADDI", reg(o[0])[1], reg(o[0])[1], None, s32(num(o[1])))
        if cmn == "nop": return ("ADDI", 0, 0, None, s32(num(o[0])) if o else 0)
        if cmn == "li": return ("ADDI", reg(o[0])[1], 0, None, s32(num(o[1])))
        if cmn == "andi": return ("ANDI", reg(o[0])[1], reg(o[0])[1], None, s32(num(o[1])))
        if cmn in ("slli", "srli", "srai"):
            return (I3[cmn], reg(o[0])[1], reg(o[0])[1], None, num(o[1]))
        if cmn in ("slli64", "srli64", "srai64"):        # shamt 0 HINTs
            return (I3[cmn[:4]], reg(o[0])[1], reg(o[0])[1], None, 0)
        if cmn == "lui":
            v = num(o[1]) & 0xfffff
            return ("LUI", reg(o[0])[1], None, None, s32(v << 12))
        if cmn == "addi16sp": return ("ADDI", 2, 2, None, s32(num(o[1])))
        if cmn == "addi4spn": return ("ADDI", reg(o[0])[1], 2, None, num(o[2]))
        if cmn in ("lw", "lwsp", "flw", "flwsp"):
            imm, b = memop(o[1])
            return ("LW" if cmn[0] == "l" else "FLW", reg(o[0])[1], b, None, imm)
        if cmn in ("sw", "swsp", "fsw", "fswsp"):
            imm, b = memop(o[1])
            return ("SW" if cmn[0] == "s" else "FSW", None, b, reg(o[0])[1], imm)
        if cmn in ("beqz", "bnez"):
            return ("BEQ" if cmn == "beqz" else "BNE", None, reg(o[0])[1], 0, s32(target(o[1], pc)))
        if cmn in ("j", "jal"):
            return ("JAL", 1 if cmn == "jal" else 0, None, None, s32(target(o[0], pc)))
        if cmn in ("jr", "jalr"):
            return ("JALR", 1 if cmn == "jalr" else 0, reg(o[0])[1], None, 0)
        if cmn == "ebreak": return ("EBREAK", None, None, None, None)
        raise ValueError("unknown compressed " + mn)
    if mn in ("lui", "auipc"):
        return (mn.upper(), reg(o[0])[1], None, None, s32((num(o[1]) & 0xfffff) << 12))
    if mn == "jal": return ("JAL", reg(o[0])[1], None, None, s32(target(o[1], pc)))
    if mn == "jalr":
        imm, b = memop(o[1])
        return ("JALR", reg(o[0])[1], b, None, imm)
    if mn in ("beq", "bne", "blt", "bge", "bltu", "bgeu"):
        return (mn.upper(), None, reg(o[0])[1], reg(o[1])[1], s32(target(o[2], pc)))
    if mn in ("lb", "lh", "lw", "lbu", "lhu", "flw"):
        imm, b = memop(o[1])
        return (mn.upper(), reg(o[0])[1], b, None, imm)
    if mn in ("sb", "sh", "sw", "fsw"):
        imm, b = memop(o[1])
        return (mn.upper(), None, b, reg(o[0])[1], imm)
    if mn in ("ecall", "ebreak", "mret", "wfi"):
        return (mn.upper(), None, None, None, None)
    if mn in ("fence", "fence.i"):
        return ("NOP", None, None, None, None)
    if mn.startswith("cbo."):
        imm, b = memop(o[0])
        return ("CBO", None, b, None, {"cbo.inval": 0, "cbo.clean": 1, "cbo.flush": 2}[mn])
    if mn in ("csrrw", "csrrs", "csrrc", "csrrwi", "csrrsi", "csrrci"):
        c = csr_num[o[1]] if o[1] in csr_num else num(o[1])
        src = num(o[2]) if mn.endswith("i") else reg(o[2])[1]
        return (mn.upper(), reg(o[0])[1], src, None, c)
    if mn == "lr.w": return ("LR", reg(o[0])[1], memop(o[1])[1], None, None)
    if mn in ("sc.w", "amoswap.w", "amoadd.w", "amoxor.w", "amoand.w", "amoor.w", "amomin.w", "amomax.w",
              "amominu.w", "amomaxu.w"):
        return (mn[:-2].upper(), reg(o[0])[1], memop(o[2])[1], reg(o[1])[1], None)
    FA = {"fadd.s": "FADD", "fsub.s": "FSUB", "fmul.s": "FMUL", "fdiv.s": "FDIV"}
    if mn in FA:
        rm = RM[o[3]] if len(o) > 3 else 7
        return (FA[mn], reg(o[0])[1], reg(o[1])[1], reg(o[2])[1], rm)
    if mn == "fsqrt.s":
        rm = RM[o[2]] if len(o) > 2 else 7
        return ("FSQRT", reg(o[0])[1], reg(o[1])[1], None, rm)
    F3 = {"fsgnj.s": "FSGNJ", "fsgnjn.s": "FSGNJN", "fsgnjx.s": "FSGNJX", "fmin.s": "FMIN", "fmax.s": "FMAX",
          "feq.s": "FEQ", "flt.s": "FLT", "fle.s": "FLE"}
    if mn in F3:
        return (F3[mn], reg(o[0])[1], reg(o[1])[1], reg(o[2])[1], None)
    FC = {"fcvt.w.s": "FCVT_W", "fcvt.wu.s": "FCVT_WU", "fcvt.s.w": "FCVT_SW", "fcvt.s.wu": "FCVT_SWU"}
    if mn in FC:
        rm = RM[o[2]] if len(o) > 2 else 7
        return (FC[mn], reg(o[0])[1], reg(o[1])[1], None, rm)
    if mn in ("fmv.x.w", "fmv.w.x", "fclass.s"):
        return ({"fmv.x.w": "FMV_XW", "fmv.w.x": "FMV_WX", "fclass.s": "FCLASS"}[mn], reg(o[0])[1],
                reg(o[1])[1], None, None)
    FM = {"fmadd.s": "FMADD", "fmsub.s": "FMSUB", "fnmsub.s": "FNMSUB", "fnmadd.s": "FNMADD"}
    if mn in FM:
        rm = RM[o[4]] if len(o) > 4 else 7
        return (FM[mn], reg(o[0])[1], reg(o[1])[1], reg(o[2])[1], reg(o[3])[1] | (rm << 8))
    raise ValueError("unhandled mnemonic " + mn)

ILLEGAL_MN = (".word", ".short", ".4byte", ".2byte", ".insn", "unimp", "c.unimp", ".byte")

od = objdump_lines(os.path.join(out, dis_name))
stats = {"match": 0, "both_illegal": 0, "mismatch": 0, "known": 0}
known_kinds = {}
bad = []
for line in open(os.path.join(out, qsim_name)):
    a, w, name, rd, rs1, rs2, imm, ok = line.split()
    pc, w = int(a, 16), int(w, 16)
    if slot_check and (pc - BASE) % 4:
        continue                                  # slot padding
    rd, rs1, rs2, imm, ok = int(rd), int(rs1), int(rs2), int(imm), int(ok)
    o = od.get(pc - BASE)
    if o is None:
        bad.append("no objdump line for %08x" % pc); stats["mismatch"] += 1; continue
    ow, mn, ops = o
    q_illegal = not ok
    o_illegal = mn in ILLEGAL_MN
    if q_illegal and o_illegal:
        stats["both_illegal"] += 1; continue
    # documented differences
    if o_illegal and not q_illegal and name == "CBO":
        # the Quake ELF's Tag_RISCV_arch lacks zicbom (cbo.* are emitted with .insn),
        # so objdump will not name them there
        stats["known"] += 1; known_kinds["cbo-no-arch-attr"] = known_kinds.get("cbo-no-arch-attr", 0) + 1
        continue
    # Documented differences (see README "decoder cross-check"):
    known = None
    if q_illegal and mn in ("c.slli", "c.srli", "c.srai") and (w >> 12) & 1:
        known = "rv32c-shamt5"          # binutils accepts shamt[5]=1 on RV32C; the spec reserves it
    elif q_illegal and mn in ("slli", "srli", "srai") and (w >> 25) & 1:
        known = "rv64-shamt"            # RV64 shift amount; illegal on RV32
    elif q_illegal and "unknown" in ops:
        known = "reserved-rm"           # rm 5/6: illegal in the spec and in VexiiRiscv (DecoderPlugin NEED_RM)
    elif q_illegal and mn in ("sret", "uret", "sfence.vma", "hfence.vvma", "hfence.gvma", "mnret", "dret"):
        known = "no-s-mode"             # privileged returns for modes the core lacks
    elif q_illegal and mn == "c.addi16sp" and ops.endswith(",0"):
        known = "c.addi16sp-0"          # reserved encoding
    elif name == "NOP" and o_illegal and (w & 0x707f) in (0x000f, 0x100f):
        known = "fence-fields"          # VexiiRiscv Rvi.FENCE/FENCE_I masks ignore rd/rs1/imm
    if known:
        stats["known"] += 1
        known_kinds[known] = known_kinds.get(known, 0) + 1
        continue
    m_aq = re.match(r"^((?:lr|sc|amo\w+)\.w)\.(aq|rl|aqrl)$", mn)
    if m_aq:
        mn = m_aq.group(1)              # bits 26:25 are don't-care in VexiiRiscv Rvi.scala:97-115
    if q_illegal != o_illegal:
        bad.append("%08x %08x legality: qsim=%s objdump=%s %s" % (pc, w, "illegal" if q_illegal else name, mn, ops))
        stats["mismatch"] += 1; continue
    try:
        c = canon(mn, ops, pc - BASE, w)
    except Exception as ex:
        bad.append("%08x %08x parse error %s: %s %s" % (pc, w, ex, mn, ops)); stats["mismatch"] += 1; continue
    op, crd, crs1, crs2, cimm = c
    # qsim representation details
    q = {"op": name, "rd": rd, "rs1": rs1, "rs2": rs2, "imm": imm}
    if name in ("SLLI", "SRLI", "SRAI"): q["imm"] = imm & 31
    if name in ("LUI", "AUIPC"): q["imm"] = s32(imm)
    if name in ("FADD", "FSUB", "FMUL", "FDIV", "FSQRT", "FCVT_W", "FCVT_WU", "FCVT_SW", "FCVT_SWU"): q["imm"] = imm & 7
    if name == "FSQRT" or name.startswith("FCVT") or name in ("FMV_XW", "FMV_WX", "FCLASS"): pass
    if name.startswith("CSR"):
        q["imm"] = imm & 0xfff
    mism = []
    if op != name: mism.append("op %s!=%s" % (op, name))
    for k, v in (("rd", crd), ("rs1", crs1), ("rs2", crs2), ("imm", cimm)):
        if v is None: continue
        if q[k] != v: mism.append("%s %s!=%s" % (k, v, q[k]))
    if mism:
        bad.append("%08x %08x %s %s -> %s" % (pc, w, mn, ops, "; ".join(mism))); stats["mismatch"] += 1
    else:
        stats["match"] += 1
for b in bad[:40]:
    print("MISMATCH", b)
print("known differences:", ", ".join("%s=%d" % kv for kv in sorted(known_kinds.items())))
print("decode-xcheck: %d words, %d match, %d both illegal, %d known binutils differences, %d mismatches" %
      (sum(stats.values()), stats["match"], stats["both_illegal"], stats["known"], stats["mismatch"]))
sys.exit(1 if stats["mismatch"] else 0)
