"""The launcher's art, rasterized from our own SVGs in assets/launcher/ (drawn for this project; nothing from the
game): the cover (the window's header), the splash and the app icon. Microsoft Edge, headless, draws the SVGs, so
the result is what the design shows.

    python -I tools/gen_launcher_art.py        (writes assets/launcher/cover.png, splash.png, flamingo.ico)

runtime/app/flamingo.rc puts the three in FlamingoPort.exe.
"""
import os
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART = os.path.join(ROOT, "assets", "launcher")
EDGE = [os.path.join(os.environ.get(v, ""), r"Microsoft\Edge\Application\msedge.exe")
        for v in ("ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA")]


def edge() -> str:
    for p in EDGE:
        if os.path.isfile(p):
            return p
    sys.exit("Microsoft Edge wasn't found (it draws the SVGs).")


def render(svg_text: str, width: int, height: int, scale: float, out: str, tmp: str) -> None:
    """The SVG at width x height CSS pixels, times `scale`, as a PNG with a transparent background."""
    page = os.path.join(tmp, "page.html")
    with open(page, "w", encoding="utf-8") as f:
        f.write("<!doctype html><html><head><style>html,body{margin:0;background:transparent;overflow:hidden}"
                f"svg{{display:block;width:{width}px;height:{height}px}}</style></head><body>{svg_text}</body></html>")
    profile = os.path.join(tmp, "profile")
    subprocess.run([edge(), "--headless", "--disable-gpu", "--hide-scrollbars", "--no-first-run",
                    f"--user-data-dir={profile}", "--default-background-color=00000000",
                    f"--force-device-scale-factor={scale}", f"--window-size={width},{height}",
                    f"--screenshot={out}", "file:///" + page.replace("\\", "/")],
                   check=True, capture_output=True, timeout=120)
    # msedge.exe hands the work to processes of its own and returns first: wait for the file to be written
    last = -1
    for _ in range(600):
        size = os.path.getsize(out) if os.path.isfile(out) else -1
        if size > 0 and size == last:
            return
        last = size
        time.sleep(0.2)
    sys.exit(f"Edge didn't write {out}")


def read(name: str) -> str:
    with open(os.path.join(ART, name), encoding="utf-8") as f:
        return f.read()


def splash_svg(cover: str) -> str:
    """The splash (640x360): the cover's scene around the flamingo, enlarged (a 16:9 cut of its 1280x200)."""
    return cover.replace('viewBox="0 0 1280 200" width="1280" height="200"',
                         'viewBox="300 0 356 200" width="640" height="360" preserveAspectRatio="xMidYMid slice"', 1)


def ico(pngs: list, out: str) -> None:
    """An .ico of PNG images (Vista and later read PNG entries at every size)."""
    entries, data = b"", b""
    offset = 6 + 16 * len(pngs)
    for size, blob in pngs:
        b = size if size < 256 else 0
        entries += struct.pack("<BBBBHHII", b, b, 0, 0, 1, 32, len(blob), offset + len(data))
        data += blob
    with open(out, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(pngs)) + entries + data)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="fp_art_", ignore_cleanup_errors=True) as tmp:
        cover = read("cover.svg")
        for n in ("cover.png", "splash.png"):
            if os.path.isfile(os.path.join(ART, n)):
                os.remove(os.path.join(ART, n))
        render(cover, 1280, 200, 3, os.path.join(ART, "cover.png"), tmp)
        render(splash_svg(cover), 640, 360, 2, os.path.join(ART, "splash.png"), tmp)
        pngs = []
        for size in (256, 128, 64, 48, 32, 24, 20, 16):
            svg = read("icon.svg" if size >= 48 else "icon_small.svg")
            base = 256 if size >= 48 else 32
            p = os.path.join(tmp, f"icon{size}.png")
            render(svg, base, base, size / base, p, tmp)
            with open(p, "rb") as f:
                pngs.append((size, f.read()))
        ico(pngs, os.path.join(ART, "flamingo.ico"))
    for n in ("cover.png", "splash.png", "flamingo.ico"):
        print(n, os.path.getsize(os.path.join(ART, n)), "bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
