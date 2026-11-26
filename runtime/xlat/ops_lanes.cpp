// Cross-lane and workgroup instructions: lane reads/writes and permutes as subgroup shuffles, the lane-mask bit
// counts (mbcnt), barriers, and the mesh shader's output request (rdna2spirv.Gen._insn's branches).
#include "ops.h"

namespace fp::xlat::ops {

void readfirstlane(const Op& o) {
    // one lane per invocation: the first lane is this lane
    const Arg& d = at(o.a, 0);
    S v = o.g.src_u(at(o.a, 1));
    o.g.dst(d, v);
}

void mbcnt(const Op& o) {
    // Count of set bits in the mask for lanes below this one (lo: lanes 0-31, hi: lanes 32-63), plus src1.
    Gen& g = o.g;
    const bool lo_form = ends(o.op, "lo_u32_b32");
    g.uses_subgroup = true;
    S mask = g.src_u(at(o.a, 1));
    S acc = g.src_u(at(o.a, 2));
    auto t = tmps(g, 8);
    const S &lane = t[0], &rel = t[1], &below = t[2], &inrange = t[3], &one = t[4], &m1 = t[5], &cnt = t[6],
            &r = t[7];
    if (g.mesh) {
        // NGG waves are wave64: the lane is the invocation's index in its 64 (the PC's subgroups are 32 wide)
        S li = g.tmp();
        g.emit({li, " = OpLoad %uint %ms_index"});
        g.emit({lane, " = OpBitwiseAnd %uint ", li, " ", g.cu(63)});
    } else {
        g.emit({lane, " = OpLoad %uint %sg_lane"});
    }
    if (lo_form)
        g.emit({rel, " = OpCopyObject %uint ", lane});
    else
        g.emit({rel, " = OpISub %uint ", lane, " ", g.cu(32)});
    // bits below `rel`, clamped to [0, 32]
    g.emit({inrange, " = OpULessThan %bool ", rel, " ", g.cu(32)});
    g.emit({one, " = OpShiftLeftLogical %uint ", g.cu(1), " ", rel});
    g.emit({m1, " = OpISub %uint ", one, " ", g.cu(1)});
    S big = g.tmp();
    // lo: lanes >= 32 count all 32 bits; hi: lanes < 32 count none (rel wraps above 32).
    g.emit({big, " = OpSelect %uint ", inrange, " ", m1, " ", lo_form ? g.cu(0xFFFFFFFF) : g.cu(0)});
    if (ends(o.op, "hi_u32_b32")) {
        S lt32 = g.tmp(), fixed = g.tmp();
        g.emit({lt32, " = OpULessThan %bool ", lane, " ", g.cu(32)});
        g.emit({fixed, " = OpSelect %uint ", lt32, " ", g.cu(0), " ", big});
        big = fixed;
    }
    g.emit({below, " = OpBitwiseAnd %uint ", mask, " ", big});
    g.emit({cnt, " = OpBitCount %uint ", below});
    g.emit({r, " = OpIAdd %uint ", cnt, " ", acc});
    g.dst(at(o.a, 0), r);
}

void sendmsg(const Op& o) {
    // NGG: GS_ALLOC_REQ asks for the subgroup's outputs, m0 = primitives << 12 | vertices. Kept in groupshared; the
    // epilogue calls SetMeshOutputCounts once, as D3D12 requires (assemble()). Other messages (GS done, dealloc) have
    // no PC counterpart.
    Gen& g = o.g;
    std::vector<S> ts;
    for (const Arg& x : o.a) ts.push_back(x.t);
    S raw = join(o.mods, " ") + " " + join(ts, " ");
    if (contains(raw, "GS_ALLOC_REQ")) {
        S m0 = g.tmp();
        g.emit({m0, " = OpLoad %uint %m0"});
        g.emit({"OpStore %ms_counts ", m0});
    }
}

void barrier(const Op& o) {
    // workgroup execution and LDS/memory barrier (GroupMemoryBarrierWithGroupSync)
    Gen& g = o.g;
    if (g.mesh && !g.lds_dwords) {
        // A mesh program's barriers order its two waves around the output request (the wave that sends GS_ALLOC_REQ,
        // then both). Here every lane is an invocation following its own branches, so a barrier in a block only some
        // of them reach is one the PC waits on for good (it removed the device: MS_VS_PedTransformPreSkinD). With no
        // LDS there is nothing for it to order: the request goes through groupshared, read after the epilogue's own
        // barrier.
        return;
    }
    g.emit({"OpControlBarrier ", g.cu(2), " ", g.cu(2), " ", g.cu(0x108)});
}

void readlane(const Op& o) {
    // sD = vS[lane]: a subgroup shuffle from that lane (the result is uniform).
    Gen& g = o.g;
    g.uses_subgroup = true;
    S v = g.src_u(at(o.a, 1));
    S ln = g.src_u(at(o.a, 2));
    S m = g.tmp(), r = g.tmp();
    g.emit({m, " = OpBitwiseAnd %uint ", ln, " ", g.cu(63)});
    g.emit({r, " = OpGroupNonUniformShuffle %uint %c3 ", v, " ", m});
    g.dst(at(o.a, 0), r);
}

void writelane(const Op& o) {
    // vD[lane] = sS: only the named lane changes (independent of EXEC, as on hardware).
    Gen& g = o.g;
    g.uses_subgroup = true;
    S val = g.src_u(at(o.a, 1));
    S ln = g.src_u(at(o.a, 2));
    auto t = tmps(g, 5);
    const S &me = t[0], &m = t[1], &eq = t[2], &old = t[3], &r = t[4];
    g.emit({me, " = OpLoad %uint %sg_lane"});
    g.emit({m, " = OpBitwiseAnd %uint ", ln, " ", g.cu(63)});
    g.emit({eq, " = OpIEqual %bool ", me, " ", m});
    S p = g.tmp();
    g.emit({p, " = OpAccessChain %_ptr_Function_uint %vgpr ", g.cu(reg(at(o.a, 0)))});
    g.emit({old, " = OpLoad %uint ", p});
    g.emit({r, " = OpSelect %uint ", eq, " ", val, " ", old});
    g.emit({"OpStore ", p, " ", r});
}

void permlane(const Op& o) {
    // Lane l reads lane sel[l & 15] of (x16: the other 16-lane half of its row; plain: the same half). The 4-bit
    // selectors for lanes 0-7 are in src1, for lanes 8-15 in src2.
    Gen& g = o.g;
    g.uses_subgroup = true;
    S v = g.src_u(at(o.a, 1));
    S s1 = g.src_u(at(o.a, 2));
    S s2 = g.src_u(at(o.a, 3));
    auto t = tmps(g, 9);
    const S &lane = t[0], &within = t[1], &hi8 = t[2], &sh = t[3], &nib1 = t[4], &nib2 = t[5], &nib = t[6],
            &row = t[7], &src = t[8];
    g.emit({lane, " = OpLoad %uint %sg_lane"});
    g.emit({within, " = OpBitwiseAnd %uint ", lane, " ", g.cu(15)});
    g.emit({hi8, " = OpUGreaterThanEqual %bool ", within, " ", g.cu(8)});
    S w7 = g.tmp();
    g.emit({w7, " = OpBitwiseAnd %uint ", within, " ", g.cu(7)});
    g.emit({sh, " = OpIMul %uint ", w7, " ", g.cu(4)});
    g.emit({nib1, " = OpBitFieldUExtract %uint ", s1, " ", sh, " ", g.cu(4)});
    g.emit({nib2, " = OpBitFieldUExtract %uint ", s2, " ", sh, " ", g.cu(4)});
    g.emit({nib, " = OpSelect %uint ", hi8, " ", nib2, " ", nib1});
    S half = g.tmp();
    g.emit({half, " = OpBitwiseAnd %uint ", lane, " ", g.cu(0xFFFFFFF0)});
    if (o.op == "v_permlanex16_b32")
        g.emit({row, " = OpBitwiseXor %uint ", half, " ", g.cu(16)});
    else
        g.emit({row, " = OpCopyObject %uint ", half});
    g.emit({src, " = OpBitwiseOr %uint ", row, " ", nib});
    S r = g.tmp();
    g.emit({r, " = OpGroupNonUniformShuffle %uint %c3 ", v, " ", src});
    g.dst(at(o.a, 0), r);
}

}  // namespace fp::xlat::ops
