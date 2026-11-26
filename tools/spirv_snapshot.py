"""Snapshot the translated shader corpus and list what a translator change altered.

    python -I tools/spirv_snapshot.py save <work>/shaders/spirv <work>/shaders/snap_before.json
    (change rdna2spirv.py, rerun it into the same folder)
    python -I tools/spirv_snapshot.py diff <work>/shaders/snap_before.json <work>/shaders/spirv

A snapshot maps each shader name to the SHA-256 of its .spvasm. `diff` lists shaders whose output changed, that
translate now but didn't before, and that no longer translate. Keep snapshots under <work>: they're made from game
shaders and are never committed. rdna2spirv.py only writes files, so clear the spirv folder before rerunning it,
or shaders it now rejects keep their old output.
"""
import hashlib
import json
import os
import sys


def snapshot(folder: str) -> dict:
    out = {}
    for f in sorted(os.listdir(folder)):
        if f.endswith(".spvasm"):
            with open(os.path.join(folder, f), "rb") as fh:
                out[f[:-7]] = hashlib.sha256(fh.read()).hexdigest()
    return out


def show(title: str, names: list, limit: int = 40) -> None:
    print(f"{title}: {len(names)}")
    for n in names[:limit]:
        print("  " + n)
    if len(names) > limit:
        print(f"  ... and {len(names) - limit} more")


def main() -> int:
    if len(sys.argv) != 4 or sys.argv[1] not in ("save", "diff"):
        print(__doc__)
        return 2
    if sys.argv[1] == "save":
        snap = snapshot(sys.argv[2])
        with open(sys.argv[3], "w", encoding="utf-8") as f:
            json.dump(snap, f, indent=0, sort_keys=True)
        print(f"{len(snap)} shaders saved to {sys.argv[3]}")
        return 0
    with open(sys.argv[2], encoding="utf-8") as f:
        old = json.load(f)
    new = snapshot(sys.argv[3])
    changed = sorted(n for n in old.keys() & new.keys() if old[n] != new[n])
    show("changed output", changed)
    show("newly translated", sorted(new.keys() - old.keys()))
    show("no longer translated", sorted(old.keys() - new.keys()))
    print(f"unchanged: {len(old.keys() & new.keys()) - len(changed)}")
    return 1 if changed or old.keys() - new.keys() else 0


if __name__ == "__main__":
    sys.exit(main())
