"""Parse LLVM AMDGPU disassembly (.s from shader_disasm.py) into a structured instruction list.

Each instruction: {"op": mnemonic, "args": [operand...], "mods": [modifier...], "enc": bytes, "pc": byte offset}
Operand kinds: sgpr s5 / s[4:7], vgpr v3 / v[0:3], special (vcc, exec, m0, scc...), inline constant, literal,
label/offset (for branches), hw register, and raw text for anything else.

As a module: parse_file(path) -> {"name", "code_start", "insns": [...], "invalid": [pc...]}
As a tool:  python -I shader_ir.py <disasm dir> [--limit N]   (parses everything and reports coverage)
"""
import os
import re
import sys
from collections import Counter

ENC = re.compile(r";\s*encoding:\s*\[([^\]]*)\]")
HDR = re.compile(r"^;\s*(\S+)\s+code 0x([0-9a-f]+)\.\.0x([0-9a-f]+)")
SPECIAL = {"vcc", "vcc_lo", "vcc_hi", "exec", "exec_lo", "exec_hi", "m0", "scc", "null", "off", "src_scc",
           "src_execz", "src_vccz", "src_lds_direct", "src_shared_base", "src_shared_limit",
           "src_private_base", "src_private_limit", "src_pops_exiting_wave_id", "lds", "flat_scratch",
           "flat_scratch_lo", "flat_scratch_hi", "ttmp", "tba", "tma"}


def operand(tok):
    t = tok.strip()
    neg = t.startswith("-")
    core = t[1:] if neg else t
    absm = core.startswith("|") and core.endswith("|")
    if absm:
        core = core[1:-1]
    m = re.fullmatch(r"sext\((.+)\)", core)
    if m:  # SDWA integer sign-extension of the selected source bits
        inner = operand(m.group(1))
        inner["sext"] = True
        inner["neg"] = inner.get("neg", False) or neg
        return inner
    m = re.fullmatch(r"neg\((.+)\)", core)
    if m:  # a negated operand written out (inline constants print this way): the float sign bit flipped
        inner = operand(m.group(1))
        if inner["k"] in ("imm", "fimm", "vgpr", "sgpr"):
            inner["neg"] = not inner.get("neg", False)
            inner.setdefault("abs", False)
            return inner
    m = re.fullmatch(r"([sv])(\d+)", core)
    if m:
        return {"k": m.group(1) + "gpr", "r": int(m.group(2)), "n": 1, "neg": neg, "abs": absm}
    m = re.fullmatch(r"([sv])\[(\d+):(\d+)\]", core)
    if m:
        lo, hi = int(m.group(2)), int(m.group(3))
        return {"k": m.group(1) + "gpr", "r": lo, "n": hi - lo + 1, "neg": neg, "abs": absm}
    m = re.fullmatch(r"ttmp(\d+)|ttmp\[(\d+):(\d+)\]", core)
    if m:
        return {"k": "ttmp", "t": core}
    m = re.fullmatch(r"\[(v\d+(?:,\s*v\d+)+)\]", core)
    if m:  # NSA image address: non-contiguous VGPR list
        return {"k": "vlist", "r": [int(x.strip()[1:]) for x in m.group(1).split(",")]}
    m = re.fullmatch(r"attr(\d+)\.([xyzw])", core)
    if m:
        return {"k": "attr", "a": int(m.group(1)), "c": "xyzw".index(m.group(2))}
    if core in ("p0", "p10", "p20"):
        return {"k": "interp", "t": core}
    m = re.fullmatch(r"(pos|param|mrt|mrtz|null|prim)(\d*)", core)
    if m and core not in ("null",):
        return {"k": "target", "t": core}
    if core in SPECIAL:
        return {"k": "special", "t": core, "neg": neg, "abs": absm}
    if re.fullmatch(r"-?(0x[0-9a-fA-F]+|\d+)", core):
        v = int(core, 0)
        return {"k": "imm", "v": -v if neg else v}
    if re.fullmatch(r"-?\d+\.\d+(e[+-]?\d+)?", core):
        return {"k": "fimm", "v": float(core) * (-1 if neg else 1), "abs": absm}
    if core.startswith("hwreg(") or core.startswith("sendmsg(") or core.startswith("vmcnt(") or \
            core.startswith("expcnt(") or core.startswith("lgkmcnt(") or core.startswith("vscnt(") or \
            core.startswith("depctr") or core.startswith("instid") or core.startswith("quad_perm") or \
            core.startswith("row_") or core.startswith("dpp") or core.startswith("sel:") or "(" in core:
        return {"k": "special", "t": core}
    return {"k": "raw", "t": t}


def split_args(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return out


SENDMSG = re.compile(r"sendmsg\([^)]*\)")

MODWORDS = re.compile(r"\b(offset:\S+|offen|idxen|glc|slc|dlc|lds|tfe|unorm|da|d16|a16|nv|r128|lwe|clamp|"
                      r"mul:\d+|div:\d+|op_sel(?:_hi)?:\[[^\]]*\]|neg_lo:\[[^\]]*\]|neg_hi:\[[^\]]*\]|"
                      r"dmask:\S+|dim:\S+|format:\S+|done|vm|compr|row_mask:\S+|bank_mask:\S+|"
                      r"bound_ctrl:\S+|fi:\S+|offset0:\S+|offset1:\S+|gds|inst_offset:\S+|"
                      r"dst_sel:\S+|dst_unused:\S+|src0_sel:\S+|src1_sel:\S+|high|dpp8:\[[^\]]*\]|"
                      r"row_mirror|row_half_mirror|row_shl:\d+|row_shr:\d+|row_ror:\d+|row_share:\d+|"
                      r"row_xmask:\d+|quad_perm:\[[^\]]*\])(?=\s|,|$)")


def parse_line(line):
    code, _, comment = line.partition(";")
    enc = None
    m = ENC.search(line)
    if m:
        enc = bytes(int(x, 16) for x in m.group(1).split(",") if x.strip())
    code = code.strip()
    if not code:
        return None
    op, _, rest = code.partition(" ")
    # sendmsg(MSG_GS, GS_OP_NOP) contains a comma: keep it as one operand.
    rest = SENDMSG.sub(lambda m: m.group(0).replace(", ", "|").replace(",", "|"), rest)
    mods = MODWORDS.findall(rest)
    rest = MODWORDS.sub("", rest)
    parts = [a for a in split_args(rest) if a.strip()]
    # 'exp pos0 v3, ...' / 'exp null off, ...': the target is space-separated from the first operand.
    if parts and len(parts[0].split()) == 2:
        parts = parts[0].split() + parts[1:]
    args = [operand(a) for a in parts]
    return {"op": op, "args": args, "mods": mods, "enc": enc.hex() if enc else None}


def parse_file(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    info = {"name": os.path.splitext(os.path.basename(path))[0], "insns": [], "invalid": 0}
    m = HDR.match(lines[0]) if lines else None
    pc = int(m.group(2), 16) if m else 0
    info["code_start"] = pc
    for ln in lines[1:]:
        if ln.startswith("; ---"):
            break
        if not ln.startswith("\t"):
            continue
        ins = parse_line(ln)
        if not ins:
            continue
        ins["pc"] = pc
        if ins["enc"]:
            pc += len(ins["enc"]) // 2
        info["insns"].append(ins)
    tail = "\n".join(lines)
    info["invalid"] = tail.count("invalid instruction encoding")
    return info


def main():
    src = sys.argv[1]
    limit = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[2] == "--limit" else None
    files = sorted(f for f in os.listdir(src) if f.endswith(".s"))[:limit]
    kinds, raw, n_ins, no_enc = Counter(), Counter(), 0, 0
    for f in files:
        info = parse_file(os.path.join(src, f))
        for ins in info["insns"]:
            n_ins += 1
            no_enc += ins["enc"] is None
            for a in ins["args"]:
                kinds[a["k"]] += 1
                if a["k"] == "raw":
                    raw[a["t"]] += 1
    print(f"{len(files)} shaders, {n_ins:,} instructions, {no_enc} without encoding bytes")
    print("operand kinds:", dict(kinds.most_common()))
    print("unparsed (raw) operands:", raw.most_common(15))


if __name__ == "__main__":
    sys.exit(main())
