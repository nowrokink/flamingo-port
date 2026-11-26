"""Translate the shaders the runtime found at run time and saved for translation.

Some of the title's pipelines (slot 86 stream pipelines) carry console code that isn't in shaders.rpf: DXBC
containers whose DXIL part has no bitcode, only XDAT -> XBCH -> FSSM, the same format as shaders.rpf's. The runtime
saves each one it can't find in the cache as <cache>/pending/<HASH digest>.dxbc; this runs them through the same
chain as shaders.rpf's (an index with each one's stage, fssm_unpack, shader_disasm, dxil_cache), so their DXIL lands
in the cache under the same digest names and the next run finds it.

Usage:
    python -I pending_shaders.py <cache folder> [--jobs N]
"""
import argparse
import json
import os
import struct
import subprocess
import sys

STAGES = {0: "ps", 1: "vs", 2: "gs", 3: "hs", 4: "ds", 5: "cs", 6: "lib", 13: "ms", 14: "as"}
HERE = os.path.dirname(os.path.abspath(__file__))


def stage_of(dxbc: bytes) -> str:
    """The program kind from the DXIL part's program header (version u32: kind in the high half)."""
    count = struct.unpack_from("<I", dxbc, 28)[0]
    for k in range(count):
        off = struct.unpack_from("<I", dxbc, 32 + 4 * k)[0]
        if dxbc[off:off + 4] == b"DXIL":
            version = struct.unpack_from("<I", dxbc, off + 8)[0]
            return STAGES.get(version >> 16, "?")
    return "?"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("cache")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    a = ap.parse_args()
    pending = os.path.join(a.cache, "pending")
    names = sorted(f for f in os.listdir(pending) if f.endswith(".dxbc")) if os.path.isdir(pending) else []
    todo = [f for f in names if not os.path.isfile(os.path.join(a.cache, f[:-5] + ".dxil"))]
    if not todo:
        print("pending shaders: none")
        return 0
    index = []
    for f in names:
        with open(os.path.join(pending, f), "rb") as h:
            index.append({"file": f, "stage": stage_of(h.read())})
    with open(os.path.join(pending, "index.json"), "w", encoding="utf-8") as h:
        json.dump(index, h, indent=1)
    print(f"pending shaders: {len(todo)} to translate")

    def py(script, *args):
        subprocess.run([sys.executable, "-I", os.path.join(HERE, script), *args], check=True)

    py("fssm_unpack.py", pending)
    py("shader_disasm.py", os.path.join(pending, "fssm"), "--out", os.path.join(pending, "disasm"), "--mcpu", "gfx1013",
       "--jobs", str(a.jobs))
    py("dxil_cache.py", os.path.join(pending, "disasm"), pending, a.cache, "--jobs", str(a.jobs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
