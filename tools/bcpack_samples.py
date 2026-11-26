"""Copy packed BCPack chunks out of the title's texture pools (.ttpf), to check the runtime's decoder
(runtime/test/bcpack_test.cpp).

    python -I tools/bcpack_samples.py <game dir> <out dir> [--per-format N]

Each chunk becomes <out>/<pool>-<index>.f<format>.s<decoded size>.pack (the packed bytes, as they are in the pool).
bcpack_test compares the runtime's decode of each with <name>.expected, written beside it by an independent decoder.
The output is data of the title's: keep it outside the repository, or under temp/ (ignored).
"""
import argparse
import glob
import os
import struct


def chunks(path):
    with open(path, "rb") as f:
        h = f.read(24)
        if h[:4] != b"RDS5":
            return
        n = struct.unpack_from("<I", h, 4)[0]
        vals = struct.unpack_from("<%dQ" % (n - 1), f.read((n - 1) * 8))
    for i, (a, b) in enumerate(zip(vals, vals[1:])):
        start, end = (a & 0x3FFFFFF) * 256, (b & 0x3FFFFFF) * 256
        v, ve = ((a >> 30) & 0x3FFFFF) * 4096, ((b >> 30) & 0x3FFFFF) * 4096
        codec, pad = (a >> 26) & 15, a >> 52
        yield i, codec, start, end - start - pad, ve - v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game")
    ap.add_argument("out")
    ap.add_argument("--per-format", type=int, default=120, help="chunks kept per format, spread over the pools")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    found = {}
    pools = sorted(glob.glob(os.path.join(args.game, "**", "*.ttpf"), recursive=True))
    for path in pools:
        for i, codec, start, packed, size in chunks(path):
            if 1 <= codec <= 7:
                found.setdefault(codec, []).append((path, i, start, packed, size))

    total = 0
    for codec, items in sorted(found.items()):
        step = max(1, len(items) // args.per_format)
        picked = items[::step][:args.per_format]
        for path, i, start, packed, size in picked:
            pool = os.path.splitext(os.path.basename(path))[0]
            name = "%s-%d.f%d.s%d.pack" % (pool, i, codec, size)
            with open(path, "rb") as f:
                f.seek(start)
                data = f.read(packed)
            with open(os.path.join(args.out, name), "wb") as o:
                o.write(data)
            total += len(data)
        print("format %d: %d of %d chunks" % (codec, len(picked), len(items)))
    print("%.1f MiB written to %s" % (total / 2**20, args.out))


if __name__ == "__main__":
    main()
