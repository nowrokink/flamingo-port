"""List the sampler descriptors (S#) that shaders build from immediates instead of loading them.

    python -I literal_samplers.py <disasm dir or .s file> --out <work>/shaders/literal_samplers.txt

Some shaders put an S# together with s_mov of constants (design.md, "Literal samplers") and sample with it. The
game never binds those, so the runtime has to register them in the descriptor hash table itself: it reads this file
and creates one sampler per line (rt::gpu::register_literal_samplers).

Output: one S# per line, four hex dwords, sorted, no duplicates. Lines starting with '#' are comments.

The scan is conservative: within straight-line code it tracks SGPRs last written by s_mov_b32 / s_movk_i32 /
s_mov_b64 with an immediate, forgets them at every branch target (another path may reach it with other values) and
at any other write. An S# is listed only when all four dwords are known constants at the image instruction. When in
doubt a literal is missed (the shader then reads the fallback sampler), never invented.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shader_ir import parse_file  # noqa: E402

# Scalar ops whose first operand is NOT a destination.
NO_DEST_PREFIX = ("s_cmp", "s_bitcmp", "s_cbranch", "s_branch", "s_waitcnt", "s_store", "s_buffer_store",
                  "s_scratch_store", "s_dcache", "s_icache", "s_setreg", "s_ttrace", "s_set_gpr_idx")
NO_DEST = {"s_nop", "s_endpgm", "s_sendmsg", "s_sendmsghalt", "s_setprio", "s_barrier", "s_inst_prefetch",
           "s_clause", "s_sleep", "s_trap", "s_setpc_b64", "s_delay_alu", "s_denorm_mode", "s_round_mode",
           "s_setkill", "s_sethalt", "s_incperflevel", "s_decperflevel", "s_endpgm_saved", "s_code_end",
           "s_version", "s_wakeup", "s_ttracedata", "s_ttracedata_imm", "s_icache_inv", "s_waitcnt_depctr"}
BRANCHES = ("s_branch", "s_cbranch_")
# Image instructions that take an S# as their 4th operand (vdata, vaddr, T#, S#).
SAMPLING = ("image_sample", "image_gather4", "image_get_lod")


def base_op(op):
    for suf in ("_e32", "_e64", "_sdwa", "_dpp"):
        if op.endswith(suf):
            return op[: -len(suf)]
    return op


def regs(a):
    """SGPR numbers an operand covers ([] when it isn't an SGPR operand)."""
    if a.get("k") != "sgpr":
        return []
    return list(range(a["r"], a["r"] + a.get("n", 1)))


def imm_bits(a):
    """32-bit value of an immediate operand, or None."""
    if a.get("k") == "imm":
        return a["v"] & 0xFFFFFFFF
    if a.get("k") == "fimm":
        return struct.unpack("<I", struct.pack("<f", a["v"]))[0]
    return None


def branch_target(ins):
    """Byte offset a branch goes to, or None."""
    imm = next((x for x in ins["args"] if x["k"] == "imm"), None)
    if imm is None or not ins.get("enc"):
        return None
    v = imm["v"] & 0xFFFF
    return ins["pc"] + len(ins["enc"]) // 2 + 4 * (v - 0x10000 if v & 0x8000 else v)


def step(ins, known, found):
    """Applies one instruction to the SGPR constant map; records literal S#s it samples with."""
    op, args = base_op(ins["op"]), ins["args"]
    if op.startswith(SAMPLING) and len(args) >= 4:
        r = regs(args[3])
        if len(r) >= 4 and all(x in known for x in r[:4]):
            found.add(tuple(known[x] for x in r[:4]))
    if op in ("s_mov_b32", "s_movk_i32") and args and regs(args[0]) and len(args) > 1:
        v = imm_bits(args[1])
        if op == "s_movk_i32" and v is not None:
            v = ((v & 0xFFFF) - 0x10000 if v & 0x8000 else v & 0xFFFF) & 0xFFFFFFFF
        d = regs(args[0])[0]
        if v is None:
            known.pop(d, None)
        else:
            known[d] = v
        return
    if op == "s_mov_b64" and args and len(regs(args[0])) == 2 and len(args) > 1:
        lo, hi = regs(args[0])
        v = imm_bits(args[1])
        if v is None:
            known.pop(lo, None)
            known.pop(hi, None)
        else:   # a 64-bit inline/literal constant is the 32-bit value sign-extended
            known[lo] = v
            known[hi] = 0xFFFFFFFF if v & 0x80000000 else 0
        return
    written = []
    if op.startswith("s_"):
        if args and op not in NO_DEST and not op.startswith(NO_DEST_PREFIX):
            written = regs(args[0])
    elif op.startswith("v_"):
        written = regs(args[0]) if args else []          # v_cmp_*_e64 sdst, v_readlane/readfirstlane
        if len(args) > 1 and ("_co_" in op or op.startswith(("v_div_scale", "v_mad_u64", "v_mad_i64"))):
            written += regs(args[1])                     # VOP3b carry-out / sdst in second position
    for x in written:
        known.pop(x, None)


def scan(insns):
    """Literal S#s used by image instructions in one shader, as tuples of 4 ints.

    Forward dataflow over basic blocks: a block starts with the SGPR constants every predecessor agrees on."""
    if not insns:
        return set()
    pcs = [i["pc"] for i in insns]
    leaders = {pcs[0]}
    for k, ins in enumerate(insns):
        if base_op(ins["op"]).startswith(BRANCHES):
            t = branch_target(ins)
            if t is not None:
                leaders.add(t)
            if k + 1 < len(insns):
                leaders.add(pcs[k + 1])
    starts = [k for k, pc in enumerate(pcs) if pc in leaders]
    block_of = {pcs[s]: b for b, s in enumerate(starts)}
    blocks = [insns[s:(starts[b + 1] if b + 1 < len(starts) else len(insns))] for b, s in enumerate(starts)]
    succ = []
    for b, blk in enumerate(blocks):
        last = blk[-1]
        op = base_op(last["op"])
        out = []
        if op.startswith(BRANCHES):
            t = branch_target(last)
            if t is None or t not in block_of:
                out = None          # unknown target: give up on constants after it
            else:
                out.append(block_of[t])
        if out is not None and op not in ("s_branch", "s_endpgm", "s_setpc_b64") and b + 1 < len(blocks):
            out.append(b + 1)
        succ.append(out or [])
    unknown_target = any(
        base_op(blk[-1]["op"]).startswith(BRANCHES) and branch_target(blk[-1]) not in block_of for blk in blocks)
    entry = [None] * len(blocks)     # None = not reached yet (top)
    entry[0] = {}
    scratch = set()
    work = [0]
    while work:
        b = work.pop()
        known = dict(entry[b])
        for ins in blocks[b]:
            step(ins, known, scratch)
        for s in succ[b]:
            if entry[s] is None:
                new = dict(known)
            else:
                new = {r: v for r, v in entry[s].items() if known.get(r) == v}
            if unknown_target:
                new = {}
            if new != entry[s]:
                entry[s] = new
                work.append(s)
    # Record only with the converged block entries: an early visit may have seen constants another path breaks.
    found = set()
    for b, blk in enumerate(blocks):
        if entry[b] is None:
            continue
        known = dict(entry[b])
        for ins in blk:
            step(ins, known, found)
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    files = [a.src] if a.src.endswith(".s") else sorted(
        os.path.join(a.src, f) for f in os.listdir(a.src) if f.endswith(".s"))
    allf, users = set(), 0
    for p in files:
        s = scan(parse_file(p)["insns"])
        users += bool(s)
        allf |= s
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# literal S#s from {len(files)} shaders ({users} use at least one)\n")
        for s in sorted(allf):
            f.write(" ".join(f"0x{w:08x}" for w in s) + "\n")
    print(f"{len(files)} shaders, {users} with literal samplers, {len(allf)} distinct S#")
    return 0


if __name__ == "__main__":
    sys.exit(main())
