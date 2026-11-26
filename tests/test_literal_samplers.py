"""Tests for tools/literal_samplers.py on hand-written disassembly (no game data).

    python -I tests/test_literal_samplers.py
"""
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import literal_samplers  # noqa: E402
from shader_ir import parse_file  # noqa: E402

SAMPLE = "image_sample v[4:7], v[2:3], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D"
DESIGN_SSHARP = (0x92, 0xC00000, 0x14500000, 0)
SET_DESIGN = ["s_mov_b32 s16, 0x92", "s_mov_b32 s17, 0xc00000", "s_mov_b32 s18, 0x14500000", "s_mov_b32 s19, 0"]


def enc(n):
    return " ; encoding: [" + ",".join(["0x00"] * n) + "]"


def scan(lines):
    """lines: instruction text, optionally ending in '@N' for its byte size (default 4)."""
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "PS_t.s")
        with open(p, "w", encoding="utf-8") as f:
            f.write("; PS_t  code 0x0..0x0 (test), gfx1013, 0 invalid words\n")
            for ln in lines:
                text, _, size = ln.partition("@")
                f.write("\t" + text.strip() + enc(int(size or 4)) + "\n")
        return literal_samplers.scan(parse_file(p)["insns"])


class LiteralSamplers(unittest.TestCase):
    def test_straight_line(self):
        self.assertEqual(scan(SET_DESIGN + [SAMPLE + " @8", "s_endpgm"]), {DESIGN_SSHARP})

    def test_loaded_word_is_not_literal(self):
        self.assertEqual(scan(SET_DESIGN[:3] + ["s_load_dword s19, s[0:1], 0x0 @8", SAMPLE + " @8"]), set())

    def test_overwritten_register(self):
        self.assertEqual(scan(SET_DESIGN + ["s_add_u32 s18, s18, 1", SAMPLE + " @8"]), set())

    def test_read_does_not_invalidate(self):
        self.assertEqual(scan(SET_DESIGN + ["s_add_u32 s0, s18, s19", "v_mov_b32 v0, s16", SAMPLE + " @8"]),
                         {DESIGN_SSHARP})

    def test_vop3b_carry_out_invalidates(self):
        self.assertEqual(scan(SET_DESIGN + ["v_add_co_u32 v0, s[18:19], v1, v2 @8", SAMPLE + " @8"]), set())

    def test_mov_b64_and_movk_sign_extension(self):
        lines = ["s_movk_i32 s16, 0x8000", "s_mov_b32 s17, 1.0", "s_mov_b64 s[18:19], -1 @4", SAMPLE + " @8"]
        self.assertEqual(scan(lines), {(0xFFFF8000, 0x3F800000, 0xFFFFFFFF, 0xFFFFFFFF)})

    def test_if_else_same_value_on_both_paths(self):
        # Byte offsets in the comments. Both paths write s19 = 0 before the join at 28.
        lines = SET_DESIGN[:3] + [
            "s_cbranch_scc0 2",          # 12 -> 16 + 8 = 24
            "s_mov_b32 s19, 0",          # 16
            "s_branch 1",                # 20 -> 24 + 4 = 28
            "s_mov_b32 s19, 0",          # 24
            SAMPLE + " @8",              # 28 (join)
        ]
        self.assertEqual(scan(lines), {DESIGN_SSHARP})

    def test_if_else_different_values(self):
        lines = SET_DESIGN[:3] + [
            "s_cbranch_scc0 2",
            "s_mov_b32 s19, 0",
            "s_branch 1",
            "s_mov_b32 s19, 5",
            SAMPLE + " @8",
        ]
        self.assertEqual(scan(lines), set())

    def test_set_before_branch_used_in_both_successors(self):
        lines = SET_DESIGN + [
            "s_cbranch_scc0 3",          # 16 -> 20 + 12 = 32
            SAMPLE + " @8",              # 20
            "s_endpgm",                  # 28
            SAMPLE + " @8",              # 32
        ]
        self.assertEqual(scan(lines), {DESIGN_SSHARP})

    def test_loop_back_edge_breaks_constant(self):
        lines = SET_DESIGN + [
            SAMPLE + " @8",              # 16: loop head, first iteration sees the constants
            "s_add_u32 s16, s16, 1",     # 24
            "s_cbranch_scc1 -4",         # 28 -> 32 - 16 = 16
        ]
        self.assertEqual(scan(lines), set())


if __name__ == "__main__":
    unittest.main(verbosity=1)
