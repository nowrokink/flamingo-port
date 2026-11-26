#!/usr/bin/env python3
"""flamingo-worker for Linux (and anywhere with Python 3.8+ and a DXC build): the same helper as flamingo_worker.cpp,
same protocol (see there and tools/dxil_cache.py). It takes HLSL from the main machine, compiles it with DXC and
sends the DXIL back; it waits for the main machine, helps while it prepares, and waits again when it's over.

Settings: worker.txt next to this file (server=http://IP:PORT, token=...), or --server / --token. DXC: --dxc, else
dxc/bin/dxc next to this file, else dxc on PATH. Use the same DXC version as the main machine (its release for Linux
needs glibc 2.38: on older systems run this in the container of tools/worker/linux/Dockerfile).
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
PROTOCOL = "1"
lock = threading.Lock()
counts = {"ok": 0, "failed": 0}
over = threading.Event()


def say(msg):
    with lock:
        print(time.strftime("%H:%M:%S") + "  " + msg, flush=True)


def first_error(text):
    for line in text.splitlines():
        if "error:" in line:
            return line.split("error:", 1)[1].strip()[:160]
    for line in text.splitlines():
        if line.strip():
            return line.strip()[:160]
    return "dxc failed"


def compile_hlsl(dxc, hlsl, profile, opt, timeout):
    """(True, DXIL) or (False, why)."""
    tmp = tempfile.mkdtemp(prefix="flamingo_")
    try:
        src, out = os.path.join(tmp, "s.hlsl"), os.path.join(tmp, "s.dxil")
        with open(src, "wb") as f:
            f.write(hlsl)
        try:
            r = subprocess.run([dxc, "-T", profile, "-E", "main", opt, src, "-Fo", out], capture_output=True,
                               text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return False, "dxc: timed out"
        except OSError as e:
            return False, f"helper: can't start dxc ({e})"
        if r.returncode == 0 and os.path.isfile(out):
            data = open(out, "rb").read()
            if data[:4] == b"DXBC":
                return True, data
        return False, "dxc: " + first_error(r.stderr + r.stdout)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def request(url, token, data=None):
    req = urllib.request.Request(url, data=data, method="POST" if data is not None else "GET",
                                 headers={"X-Protocol": PROTOCOL, "X-Token": token})
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def work(n, a, name):
    waiting = False
    while not over.is_set():
        try:
            code, headers, body = request(f"{a.server}/job?w={name}", a.token)
        except OSError:
            code, headers, body = None, {}, b""
        if code in (None, 410):
            if not waiting and n == 0:
                say("waiting for the main machine to prepare...")
            waiting = True
            time.sleep(5)
            continue
        if waiting and n == 0:
            say("helping")
        waiting = False
        if code == 403:
            if n == 0:
                say("the main machine refused the token: copy worker.txt again from the launcher")
            over.set()
            return
        if code == 426:
            if n == 0:
                say("the main machine runs another version: update this helper")
            over.set()
            return
        job, profile = headers.get("X-Job-Id"), headers.get("X-Profile")
        if code != 200 or not job or not profile:
            time.sleep(1)
            continue
        timeout = int(headers.get("X-Timeout") or a.timeout)
        ok, result = compile_hlsl(a.dxc, body, profile, headers.get("X-Opt") or "-O0", timeout)
        data = result if ok else result.encode("utf-8", "replace")
        for attempt in range(10):
            try:
                request(f"{a.server}/result?id={job}&ok={1 if ok else 0}", a.token, data)
                break
            except OSError:
                time.sleep(2)
        with lock:
            counts["ok" if ok else "failed"] += 1
            done = counts["ok"] + counts["failed"]
        if done % 25 == 0 or not ok:
            say(f"{counts['ok']} compiled, {counts['failed']} failed" + ("" if ok else f"  (last: {result})"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server")
    ap.add_argument("--token")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--dxc")
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--name", default=socket.gethostname())
    a = ap.parse_args()
    conf = os.path.join(HERE, "worker.txt")
    if os.path.isfile(conf):
        for line in open(conf, encoding="utf-8"):
            k, _, v = line.strip().partition("=")
            if k == "server" and not a.server:
                a.server = v
            elif k == "token" and not a.token:
                a.token = v
    if not a.server or not a.token:
        print("no server or token: put worker.txt (from the Flamingo Port launcher) next to this file")
        return 2
    a.server = a.server.rstrip("/")
    a.dxc = a.dxc or next((p for p in (os.path.join(HERE, "dxc", "bin", "dxc"),) if os.path.isfile(p)),
                          shutil.which("dxc"))
    if not a.dxc:
        print("dxc not found: put it in dxc/bin next to this file, on PATH, or give --dxc")
        return 1
    ok, why = compile_hlsl(a.dxc, b"float4 main() : SV_Target { return float4(1, 0, 0, 1); }\n", "ps_6_6", "-O0", 60)
    if not ok:
        print(f"DXC doesn't work here: {why}")
        return 1
    try:
        os.nice(10)
    except (AttributeError, OSError):
        pass
    say(f"flamingo-worker on {a.name}: {a.jobs} jobs, server {a.server}")
    threads = [threading.Thread(target=work, args=(n, a, a.name), daemon=True) for n in range(a.jobs)]
    for t in threads:
        t.start()
    try:
        for t in threads:
            t.join()
    except KeyboardInterrupt:
        over.set()
    say(f"done: {counts['ok']} compiled, {counts['failed']} failed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
