"""Regression tests for tools/rdna2spirv.py on hand-written shaders (no game data).

    python -I tests/test_translator.py            # run
    python -I tests/test_translator.py --update   # accept the current output as the new reference

Each case in tests/translator/cases/ is RDNA (gfx10) assembly, one instruction per line. The name prefix sets the
stage (VS_ / PS_). Directives in comments:
    ; @isg1 <sysvalue> <register> <mask> <rwmask> [ctype]   one input-signature element (PS inputs, VS fetch)
    ; @unsupported <text>                                   the translator must reject it with this reason
    ; encoding: [..]                                        bytes after an instruction; only their count matters
                                                            (it sets the PC, so branches need it)
The hand-written GPU test shaders in runtime/test/shaders/*.asm are included as PS cases.

The reference output is tests/translator/expected/<name>.spvasm (or <name>.unsupported). A difference fails the
test and prints a diff: check it, and if the change is intended, rerun with --update and commit both together.
When spirv-as and spirv-val are on PATH, every translated case is also assembled and validated (Vulkan 1.2).
This checks that output is unchanged and valid, not that values are right: the GPU harness checks values.
"""
import difflib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASES = os.path.join(ROOT, "tests", "translator", "cases")
EXPECTED = os.path.join(ROOT, "tests", "translator", "expected")
GPU_SHADERS = os.path.join(ROOT, "runtime", "test", "shaders")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import rdna2spirv  # noqa: E402

UPDATE = "--update" in sys.argv
if UPDATE:
    sys.argv.remove("--update")


def cases():
    out = []
    for f in sorted(os.listdir(CASES)):
        if f.endswith(".s"):
            out.append((f[:-2], os.path.join(CASES, f)))
    for f in sorted(os.listdir(GPU_SHADERS)):
        if f.endswith(".asm"):
            out.append(("PS_gpu_" + f[:-4], os.path.join(GPU_SHADERS, f)))
    return out


def read_case(path):
    """(instruction lines, ISG1 elements, expected rejection or None)"""
    lines, sig, unsupported = [], [], None
    with open(path, encoding="utf-8") as f:
        text = f.read()
    for raw in text.splitlines():
        s = raw.strip()
        if s.startswith("; @isg1"):
            v = [int(x, 0) for x in s.split()[2:]]
            sig.append((v + [3])[:5])          # default component type: float32
        elif s.startswith("; @unsupported"):
            unsupported = s[len("; @unsupported"):].strip()
        elif s and not s.startswith(";"):
            lines.append(s)
    return lines, sig, unsupported


def dxbc_with_isg1(sig):
    """A minimal DXBC container holding only an ISG1 chunk, laid out as rdna2spirv.input_signature reads it."""
    elems = b""
    for sysv, reg, mask, rw, ctype in sig:
        elems += struct.pack("<6I", 0, 0, 0, sysv, ctype, reg) + bytes([mask, rw]) + bytes(6)
    payload = struct.pack("<II", len(sig), 8) + elems
    chunk = b"ISG1" + struct.pack("<I", len(payload)) + payload
    head = b"DXBC" + bytes(16) + struct.pack("<III", 1, 36 + len(chunk), 1) + struct.pack("<I", 36)
    return head + chunk


def stage_disasm(name, lines, sig, work):
    """Lay the case out like the real pipeline: <work>/<name>.dxbc next to <work>/disasm/<name>.s."""
    os.makedirs(os.path.join(work, "disasm"), exist_ok=True)
    with open(os.path.join(work, name + ".dxbc"), "wb") as f:
        f.write(dxbc_with_isg1(sig))
    s = os.path.join(work, "disasm", name + ".s")
    with open(s, "w", encoding="utf-8") as f:
        f.write(f"; {name}  code 0x0..0x0 (test case), gfx1013, 0 invalid words\n")
        f.writelines("\t" + ln + "\n" for ln in lines)
    return s


def translate_case(name, path, work):
    lines, sig, unsupported = read_case(path)
    rdna2spirv._STAGES = None          # stage comes from the name prefix (there is no carve index here)
    try:
        return "ok", rdna2spirv.translate(stage_disasm(name, lines, sig, work)), unsupported
    except rdna2spirv.Unsupported as e:
        return "unsupported", str(e) + "\n", unsupported


def spirv_check(asm, work, name):
    """Assemble and validate when the SPIR-V tools are installed; None when they aren't."""
    if not (shutil.which("spirv-as") and shutil.which("spirv-val")):
        return None
    sa, sv = os.path.join(work, name + ".spvasm"), os.path.join(work, name + ".spv")
    with open(sa, "w", encoding="utf-8") as f:
        f.write(asm)
    for cmd in (["spirv-as", sa, "-o", sv, "--target-env", "vulkan1.2"],
                ["spirv-val", "--target-env", "vulkan1.2", sv]):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            return f"{cmd[0]}: {r.stderr.strip()}"
    return ""


class Translator(unittest.TestCase):
    def test_cases(self):
        found = cases()
        self.assertTrue(found, "no test cases")
        with tempfile.TemporaryDirectory() as work:
            for name, path in found:
                with self.subTest(name):
                    kind, out, want_reject = translate_case(name, path, work)
                    if want_reject is not None:
                        self.assertEqual(kind, "unsupported", f"{name} should be rejected: {want_reject}")
                        self.assertEqual(out.strip(), want_reject)
                    else:
                        self.assertEqual(kind, "ok", f"{name} was rejected: {out.strip()}")
                    ext = ".spvasm" if kind == "ok" else ".unsupported"
                    ref = os.path.join(EXPECTED, name + ext)
                    if UPDATE:
                        os.makedirs(EXPECTED, exist_ok=True)
                        for old in (".spvasm", ".unsupported"):
                            if old != ext and os.path.exists(os.path.join(EXPECTED, name + old)):
                                os.remove(os.path.join(EXPECTED, name + old))
                        with open(ref, "w", encoding="utf-8", newline="\n") as f:
                            f.write(out)
                    self.assertTrue(os.path.isfile(ref), f"no reference for {name}: run with --update")
                    with open(ref, encoding="utf-8") as f:
                        want = f.read()
                    if out != want:
                        diff = "".join(difflib.unified_diff(want.splitlines(True), out.splitlines(True),
                                                            "expected/" + name + ext, "now", n=2))
                        self.fail(f"{name} output changed:\n{diff[:6000]}")
                    if kind == "ok":
                        err = spirv_check(out, work, name)
                        if err:
                            self.fail(f"{name} is not valid SPIR-V: {err}")

    def test_no_orphan_references(self):
        names = {n for n, _ in cases()}
        orphans = [f for f in os.listdir(EXPECTED) if os.path.splitext(f)[0] not in names]
        self.assertEqual(orphans, [], "reference files without a case (delete them)")


if __name__ == "__main__":
    if not (shutil.which("spirv-as") and shutil.which("spirv-val")):
        print("note: spirv-as/spirv-val not on PATH, so outputs are compared but not validated", file=sys.stderr)
    unittest.main(verbosity=1)
