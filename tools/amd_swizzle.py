"""AMD GFX10/RDNA2 texture detiling from addrlib's published swizzle patterns.

Reads gfx10SwizzlePattern.h (Mesa's addrlib, MIT licence) and builds, for a swizzle mode and pipe/packer
configuration, the equation that maps an element's (x, y) to its byte offset. 64 KB macro blocks are laid out
row-major; inside a block every address bit is an XOR of x and y bits (including bits above the block, which is
how the XOR modes spread neighbouring blocks across pipes).

As a tool:
    python -I amd_swizzle.py <blob> --bpe 8 --width 2048 --height 2048 [--offset N] [--modes 64K] [--out best.png]
tries every 64 KB pattern table and configuration for that element size, scores each by image smoothness, and
renders the best. As a module, `detile(data, pattern, bpe, wb, hb)` returns linear blocks.
"""
import argparse
import ast
import os
import re
import sys

import numpy as np

HEADER = os.environ.get("AMD_SWIZZLE_HEADER", r"<ref>\mesa-addrlib\gfx10SwizzlePattern.h")
BPE_INDEX = {1: 0, 2: 1, 4: 2, 8: 3, 16: 4}


def _symbols():
    env = {}
    for i in range(16):
        env[f"X{i}"] = 1 << i
        env[f"Y{i}"] = 1 << (16 + i)
        env[f"Z{i}"] = 1 << (32 + i)
        env[f"S{i}"] = 1 << (48 + i)
    return env


def _pattern_value(expression, env):
    if len(expression) > 256:
        raise ValueError("swizzle expression is too long")
    try:
        tree = ast.parse(expression, mode="eval")
    except SyntaxError as exc:
        raise ValueError(f"invalid swizzle expression: {expression!r}") from exc
    if sum(1 for _ in ast.walk(tree)) > 128:
        raise ValueError("swizzle expression is too complex")

    def evaluate(node):
        if isinstance(node, ast.Expression):
            return evaluate(node.body)
        if isinstance(node, ast.Constant) and type(node.value) is int:
            value = node.value
        elif isinstance(node, ast.Name) and node.id in env:
            value = env[node.id]
        elif isinstance(node, ast.BinOp):
            left, right = evaluate(node.left), evaluate(node.right)
            if isinstance(node.op, ast.BitOr):
                value = left | right
            elif isinstance(node.op, ast.BitAnd):
                value = left & right
            elif isinstance(node.op, ast.BitXor):
                value = left ^ right
            elif isinstance(node.op, ast.LShift) and right < 64:
                value = left << right
            elif isinstance(node.op, ast.RShift) and right < 64:
                value = left >> right
            else:
                raise ValueError(f"unsupported swizzle operator in {expression!r}")
        else:
            raise ValueError(f"unsupported swizzle expression: {expression!r}")

        if not 0 <= value < (1 << 64):
            raise ValueError(f"swizzle expression is outside 64-bit range: {expression!r}")
        return value

    return evaluate(tree)


def load(path=HEADER):
    text = open(path, encoding="utf-8", errors="replace").read()
    env = _symbols()
    nibbles = {}
    for name in ("NIBBLE01", "NIBBLE2", "NIBBLE3", "NIBBLE4"):
        m = re.search(r"GFX10_SW_PATTERN_" + name + r"\[\]\[\d+\]\s*=\s*\{(.*?)\n\};", text, re.S)
        rows = re.findall(r"\{([^{}]*)\}", m.group(1))
        nibbles[name] = [[_pattern_value(t.strip() or "0", env) for t in r.split(",") if t.strip()] for r in rows]
    tables = {}
    for m in re.finditer(r"const ADDR_SW_PATINFO (GFX10_SW_\w+_PATINFO)\[\]\s*=\s*\{(.*?)\n\};", text, re.S):
        entries = []
        for row in m.group(2).splitlines():
            mm = re.match(r"\s*\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*\}\s*,\s*//\s*(.*)", row)
            if mm:
                entries.append(tuple(int(v) for v in mm.groups()[:5]) + (mm.group(6).strip(),))
        tables[m.group(1)] = entries
    return nibbles, tables


def equation(nibbles, entry, block_bits):
    """Per address bit, (x mask, y mask) for the given pattern entry, for the first `block_bits` bits."""
    _, n01, n2, n3, n4, _ = entry
    bits = list(nibbles["NIBBLE01"][n01]) + list(nibbles["NIBBLE2"][n2]) + list(nibbles["NIBBLE3"][n3]) + \
        list(nibbles["NIBBLE4"][n4])
    return [(v & 0xFFFF, (v >> 16) & 0xFFFF) for v in bits[:block_bits]]


def _parity(a):
    a = a ^ (a >> 8)
    a = a ^ (a >> 4)
    a = a ^ (a >> 2)
    a = a ^ (a >> 1)
    return a & 1


def offsets(eq, bpe, wb, hb, block_bytes=65536):
    """Byte offset of every element (row-major y, x) for a wb x hb element grid."""
    y, x = np.mgrid[0:hb, 0:wb].astype(np.int64)
    off = np.zeros_like(x)
    for bit, (mx, my) in enumerate(eq):
        if mx or my:
            off |= (_parity(x & mx) ^ _parity(y & my)) << bit
    # Macro-block size in elements, from which coordinate bits the equation uses below block_bytes.
    xb = max((mx.bit_length() for mx, _ in eq if mx), default=0)
    yb = max((my.bit_length() for _, my in eq if my), default=0)
    # The block's own extent: the x/y bits that appear as the *sole* term of some address bit.
    solo_x = [mx.bit_length() for mx, my in eq if mx and not my and mx & (mx - 1) == 0]
    solo_y = [my.bit_length() for mx, my in eq if my and not mx and my & (my - 1) == 0]
    bw = 1 << max(solo_x + [0])
    bh = 1 << max(solo_y + [0])
    blocks_x = (wb + bw - 1) // bw
    block = (y // bh) * blocks_x + (x // bw)
    return off + block * block_bytes, (bw, bh, xb, yb)


def detile(data, eq, bpe, wb, hb):
    off, _ = offsets(eq, bpe, wb, hb)
    idx = off // bpe
    raw = np.frombuffer(data[: len(data) // bpe * bpe], np.uint8).reshape(-1, bpe)
    if idx.max() >= len(raw):
        return None
    return raw[idx.reshape(-1)]


def main():
    sys.path.insert(0, os.path.dirname(__file__))
    from tile_probe import block_feature, score
    from swizzle_search import render  # noqa: F401
    ap = argparse.ArgumentParser()
    ap.add_argument("blob")
    ap.add_argument("--bpe", type=int, default=8)
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--offset", type=int, default=0)
    ap.add_argument("--modes", default="_64K_")
    ap.add_argument("--out")
    a = ap.parse_args()
    nibbles, tables = load()
    data = open(a.blob, "rb").read()[a.offset:]
    wb, hb = a.width // 4, a.height // 4
    res = []
    for tname, entries in tables.items():
        if a.modes not in tname:
            continue
        for e in entries:
            if f" {a.bpe} bpe" not in e[5]:
                continue
            eq = equation(nibbles, e, 16)
            lin = detile(data, eq, a.bpe, wb, hb)
            if lin is None:
                continue
            img = block_feature(lin, a.bpe).reshape(hb, wb, 3)
            res.append((score(img), tname, e[5], e))
    res.sort(key=lambda r: r[0])
    seen = set()
    for s, t, c, _ in res:
        if (t, c) in seen:
            continue
        seen.add((t, c))
        print(f"{s:8.2f}  {t:40} {c}")
        if len(seen) >= 10:
            break
    if a.out and res:
        import io
        import struct
        from PIL import Image
        lin = detile(data, equation(nibbles, res[0][3], 16), a.bpe, wb, hb)
        hdr = bytearray(128)
        hdr[0:4] = b"DDS "
        struct.pack_into("<IIIIIII", hdr, 4, 124, 0x1007, a.height, a.width, lin.size, 0, 1)
        struct.pack_into("<I", hdr, 108, 0x1000)
        if a.bpe == 8:
            struct.pack_into("<II4s", hdr, 76, 32, 4, b"DXT1")
            body = bytes(hdr)
        else:
            struct.pack_into("<II4s", hdr, 76, 32, 4, b"DX10")
            body = bytes(hdr) + struct.pack("<IIIII", 98, 3, 0, 1, 0)
        Image.open(io.BytesIO(body + lin.tobytes())).convert("RGB").save(a.out)
        print("best ->", a.out)


if __name__ == "__main__":
    sys.exit(main())
