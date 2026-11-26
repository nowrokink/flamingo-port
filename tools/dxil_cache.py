"""Builds the translated-shader cache the runtime reads: each disassembled shader through `rdna2spirv.py --target
d3d12` -> SPIR-V -> HLSL (SPIRV-Cross) -> DXIL (DXC), one file per shader named by the 16-byte HASH chunk digest of
its container in shaders.rpf (what a pipeline's stage metadata names it by; see docs/gpu-memory.md).

Two halves: processes here translate to HLSL (a few MB of intermediates per shader, deleted at once) and feed a
queue; DXC compiles from that queue, here and on helper PCs on the network (tools/worker, flamingo-worker.exe). DXC
is most of the time, so helpers need only DXC: no Python and no copy of the game's data, just HLSL in and DXIL out.

Writes <out>/<digest>.dxil and <out>/index.json ({digest: {name, stage, bytes}}), and a summary of what failed and
why. Shaders already in the cache are skipped, so an interrupted build continues where it stopped. A failure is
kept as <digest>.fail with the tools' identity, so it isn't retried until the translator changes.

Helpers (--serve PORT, or FLAMINGO_SERVE): plain HTTP on the local network, from private addresses only, each request
with the build's token (--token or FLAMINGO_TOKEN):
    GET  /job?w=<name>            200 with X-Job-Id, X-Profile, X-Opt and the HLSL; 204 nothing now; 410 build over
    POST /result?id=<id>&ok=1     the DXIL (ok=0: the error)
A job a helper doesn't bring back in time goes back to the queue.

Usage:
    python -I tools/dxil_cache.py <disasm dir> <carved dxbc dir> <out dir> [--jobs N] [--limit N] [--timeout S]
        [--opt 0|1|3] [--serve PORT] [--token T] [--keep DIR]   (--keep: the intermediates, for debugging)
(spirv-as, spirv-val, spirv-cross and dxc from the Vulkan SDK must be on PATH.)
"""
import argparse
import collections
import hashlib
import ipaddress
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
from collections import Counter
from concurrent.futures import ProcessPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rdna2spirv  # noqa: E402

PROFILES = {"vs": "vs_6_6", "ps": "ps_6_6", "cs": "cs_6_6", "ms": "ms_6_5"}
PROTOCOL = "1"


def tools_id():
    """Identity of the translation (the translator and this file): failures kept by another version are retried."""
    h = hashlib.sha1()
    for f in ("rdna2spirv.py", "dxil_cache.py"):
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), f), "rb") as fh:
            h.update(fh.read())
    return h.hexdigest()[:12]


TOOLS_ID = tools_id()


def hash_digest(dxbc_path):
    """The HASH chunk's 16-byte digest of a DXBC container (after its 4-byte flags), or None."""
    try:
        b = open(dxbc_path, "rb").read()
    except OSError:
        return None
    for k in range(struct.unpack_from("<I", b, 28)[0]):
        o = struct.unpack_from("<I", b, 32 + 4 * k)[0]
        if b[o:o + 4] == b"HASH":
            return b[o + 12:o + 28].hex()
    return None


def vs_inputs(dxbc_path):
    """A vertex shader's inputs from its container's ISG1: [(semantic, index, register)] without system values. The
    translated shader takes input register r as TEXCOORD<r> (tools/rdna2spirv.py: the fetch call becomes vertex
    inputs at location r); the runtime pairs the title's input layout (by semantic) with these registers."""
    try:
        b = open(dxbc_path, "rb").read()
    except OSError:
        return None
    for k in range(struct.unpack_from("<I", b, 28)[0]):
        o = struct.unpack_from("<I", b, 32 + 4 * k)[0]
        if b[o:o + 4] == b"ISG1":
            p = b[o + 8:o + 8 + struct.unpack_from("<I", b, o + 4)[0]]
            count, at = struct.unpack_from("<II", p, 0)
            out = []
            for i in range(count):
                _, name_at, index, sysv, _, reg = struct.unpack_from("<6I", p, at + 32 * i)
                if sysv == 0:
                    name = p[name_at:p.index(b"\0", name_at)].decode("ascii", "replace")
                    out.append((name, index, reg))
            return out
    return []


def write_vsin(out, digest, dxbc_path):
    """<digest>.vsin: one line per input, "SEMANTIC index register" (see vs_inputs)."""
    path = os.path.join(out, digest + ".vsin")
    if os.path.isfile(path):
        return
    inputs = vs_inputs(dxbc_path)
    if inputs is None:
        return
    with open(path, "w", encoding="ascii") as f:
        for name, index, reg in inputs:
            f.write(f"{name} {index} {reg}\n")


def first_line(text):
    """The first error message, without the file and position it starts with."""
    for line in text.splitlines():
        if "error:" in line:
            return line.split("error:", 1)[1].strip()[:160]
    for line in text.splitlines():
        if line.strip():
            return line.strip()[:160]
    return "?"


def kept_failure(path):
    """The reason of a failure kept by this version of the tools, or None."""
    try:
        with open(path, encoding="utf-8") as f:
            tid, _, reason = f.read().partition("\n")
        return reason.strip() if tid.strip() == TOOLS_ID else None
    except OSError:
        return None


# ---------------------------------------------------------------- translation (worker processes)
def init(cs_regs=None):
    rdna2spirv.set_target("d3d12")
    rdna2spirv.set_cs_regs(cs_regs)


def to_hlsl(s_path, keep):
    """(HLSL bytes, None) or (None, why not)."""
    name = os.path.splitext(os.path.basename(s_path))[0]
    tmp = keep or tempfile.mkdtemp(prefix="dxil_")
    try:
        why = rdna2spirv.translate_one(s_path, tmp)
        if why:
            return None, "translate: " + why
        spv, hlsl = os.path.join(tmp, name + ".spv"), os.path.join(tmp, name + ".hlsl")
        r = subprocess.run(["spirv-cross", "--hlsl", "--shader-model", "66", spv, "--output", hlsl],
                           capture_output=True, text=True)
        if r.returncode:
            return None, "spirv-cross: " + first_line(r.stderr)
        with open(hlsl, "rb") as f:
            return link_signatures(f.read(), rdna2spirv.stage_of(name, s_path)), None
    finally:
        if not keep:
            shutil.rmtree(tmp, ignore_errors=True)


SIGNATURE_STRUCT = {"ps": "SPIRV_Cross_Input", "vs": "SPIRV_Cross_Output", "ms": "gl_MeshPerVertexEXT"}


def link_signatures(hlsl, stage):
    """The signature a stage hands to the next (a vertex or mesh shader's outputs) or takes from the last (a pixel
    shader's inputs), laid out so that any two of them link. D3D12 matches the two by register, and DXC gives
    registers in the order of declaration. SPIRV-Cross declares the parameters a shader names, by number, and
    SV_Position last, so the position's register was the count of parameters: a pixel shader that read the position
    and fewer parameters than the stage before it wrote (a full-screen pass, mostly) had it somewhere else and the
    pipeline was refused (E_INVALIDARG; 27 of 660 pipelines of a world load). Here the position always comes first
    and the parameters follow from 0 with no number left out, so parameter n is register n + 1 on both sides:
    a pixel shader that doesn't read the position, or skips a parameter, declares them unused."""
    struct = SIGNATURE_STRUCT.get(stage)
    if not struct:
        return hlsl
    text = hlsl.decode("utf-8")
    m = re.search(r"struct " + struct + r"\r?\n\{\r?\n(.*?)\r?\n\};", text, re.S)
    if not m:
        return hlsl
    eol = "\r\n" if "\r\n" in m.group(0) else "\n"
    position, params, rest = None, {}, []
    for line in m.group(1).split(eol):
        sem = re.search(r":\s*(\w+?)(\d*);\s*$", line)
        if sem and sem.group(1) == "SV_Position":
            position = line
        elif sem and sem.group(1) == "TEXCOORD":
            params[int(sem.group(2) or 0)] = line
        else:
            rest.append(line)
    if stage == "ps":
        position = position or "    float4 _unused_position : SV_Position;"
        for n in range(max(params, default=-1)):
            params.setdefault(n, f"    float4 _unused_parameter{n} : TEXCOORD{n};")
    elif position is None or sorted(params) != list(range(len(params))):
        return hlsl         # a first stage with no position, or that leaves a parameter out: as SPIRV-Cross wrote it
    lines = [position] + [params[n] for n in sorted(params)] + rest
    return (text[:m.start(1)] + eol.join(lines) + text[m.end(1):]).encode("utf-8")


def run_dxc(hlsl, profile, opt, timeout):
    """(DXIL bytes, None) or (None, why not). DXC's optimiser takes minutes on large HLSL and the driver optimises
    again when it makes the pipeline, so the default is -O0 (a third of -O3's time on a sample)."""
    tmp = tempfile.mkdtemp(prefix="dxc_")
    try:
        src, out = os.path.join(tmp, "s.hlsl"), os.path.join(tmp, "s.dxil")
        with open(src, "wb") as f:
            f.write(hlsl)
        try:
            r = subprocess.run(["dxc", "-T", profile, "-E", "main", opt, src, "-Fo", out], capture_output=True,
                               text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return None, "dxc: timed out"
        if r.returncode:
            return None, "dxc: " + first_line(r.stderr)
        with open(out, "rb") as f:
            return f.read(), None
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ---------------------------------------------------------------- the DXC queue
class Queue:
    """HLSL waiting for DXC, taken by the local compilers and by helpers; what helpers hold is leased."""

    def __init__(self, lease):
        self.cv = threading.Condition()
        self.jobs = collections.deque()
        self.local = collections.deque()    # jobs only this machine compiles (a helper said they failed)
        self.leased = {}                    # id -> (job, deadline, helper)
        self.lease, self.next_id, self.over = lease, 1, False
        self.helpers = {}                   # helper name -> last seen
        self.by_helpers = 0
        self.by_machine = Counter()         # shaders compiled per machine ("" = this one)

    def put(self, job):
        with self.cv:
            job["id"] = self.next_id
            self.next_id += 1
            self.jobs.append(job)
            self.cv.notify()

    def put_local(self, job):
        with self.cv:
            self.local.append(job)
            self.cv.notify_all()

    def size(self):
        with self.cv:
            return len(self.jobs) + len(self.leased) + len(self.local)

    def take(self, wait, helper=None, local_only=False):
        """A job: helpers take from the queue (leased), this machine first what helpers failed; with `local_only`
        only those."""
        with self.cv:
            self._expire()
            if not helper and self.local:
                return self.local.popleft()
            if not self.jobs and wait:
                self.cv.wait(wait)
                self._expire()
            if not helper and self.local:
                return self.local.popleft()
            if not self.jobs or local_only:
                return None
            job = self.jobs.popleft()
            if helper:
                self.leased[job["id"]] = (job, time.time() + self.lease, helper)
                self.helpers[helper] = time.time()
            return job

    def give_back(self, job_id):
        """A helper's result: the job it had, or None if it came too late (the job went back to the queue)."""
        with self.cv:
            item = self.leased.pop(job_id, None)
            if item:
                self.by_helpers += 1
                item[0]["machine"] = item[2]
            return item[0] if item else None

    def _expire(self):
        now = time.time()
        for jid, (job, deadline, _) in list(self.leased.items()):
            if deadline < now:
                del self.leased[jid]
                self.jobs.appendleft(job)

    def active_helpers(self):
        """Helpers seen lately or holding a job (the largest shaders keep one busy for minutes)."""
        with self.cv:
            now = time.time()
            busy = {h for _, _, h in self.leased.values()}
            return sorted(h for h, t in self.helpers.items() if now - t < 120 or h in busy)

    def working(self):
        """What each helper holds right now: {helper: [jobs it is compiling, the longest it has held one (s)]}. A
        helper compiles as many at once as it has DXC processes, so this shows how wide each machine works; the
        waiting count says whether the helpers are short of work or the work is short of helpers."""
        with self.cv:
            now = time.time()
            held = {}
            for _, deadline, helper in self.leased.values():
                n, longest = held.get(helper, (0, 0))
                held[helper] = (n + 1, max(longest, round(now - (deadline - self.lease))))
            return {h: list(v) for h, v in held.items()}, len(self.jobs)


def private(addr):
    try:
        ip = ipaddress.ip_address(addr)
        return ip.is_private or ip.is_loopback
    except ValueError:
        return False


def serve(queue, port, token, opt, timeout, finish):
    """Helpers' way in: plain HTTP on every interface, private addresses and the token only."""
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    from urllib.parse import parse_qs, urlparse

    class Handler(BaseHTTPRequestHandler):
        def answer(self, code, body=b"", headers=()):
            self.send_response(code)
            for k, v in headers:
                self.send_header(k, v)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def allowed(self):
            if not private(self.client_address[0]) or self.headers.get("X-Token") != token:
                self.answer(403)
                return False
            if self.headers.get("X-Protocol") != PROTOCOL:
                self.answer(426)
                return False
            return True

        def do_GET(self):
            if not self.allowed():
                return
            u = urlparse(self.path)
            if u.path != "/job":
                return self.answer(404)
            if queue.over:
                return self.answer(410)
            helper = (parse_qs(u.query).get("w", ["?"])[0] or "?")[:40] + "@" + self.client_address[0]
            job = queue.take(20, helper)
            if not job:
                return self.answer(410 if queue.over else 204)
            self.answer(200, job["hlsl"], [("X-Job-Id", str(job["id"])), ("X-Profile", PROFILES[job["stage"]]),
                                           ("X-Opt", opt), ("X-Timeout", str(timeout))])

        def do_POST(self):
            if not self.allowed():
                return
            u = urlparse(self.path)
            q = parse_qs(u.query)
            n = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(n) if 0 < n <= 64 << 20 else b""
            if u.path != "/result" or not q.get("id", [""])[0].isdigit():
                return self.answer(400)
            job = queue.give_back(int(q["id"][0]))
            if job:
                if q.get("ok", ["0"])[0] == "1" and body[:4] == b"DXBC":
                    finish(job, body, None)
                else:
                    # a helper's failure isn't kept as such (its DXC may be what's broken): compiled here again
                    queue.put_local(job)
            self.answer(200)

        def log_message(self, *a):
            pass

    httpd = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


# ---------------------------------------------------------------- progress
class Progress:
    """Percentage, counts, rate and time left: a live line (and the window title) in a console, a line every 1% in
    a log, and "@@ui" events for the launcher (FLAMINGO_UI=1, see make_game.py). The rate counts only shaders built
    in this run: those already in the cache are done at once and would make the time left look short."""

    def __init__(self, total):
        self.total, self.k, self.built, self.cached = total, 0, 0, 0
        self.t0, self.last, self.sent = time.time(), -1, 0.0
        self.live = sys.stdout.isatty()
        self.ui = os.environ.get("FLAMINGO_UI") == "1"
        self.lock = threading.Lock()
        self.extra = {}
        # <cache>/translation.json, written about once a second: the same numbers as the "@@ui" events, for a
        # launcher that didn't start this run (it shows the progress and the helpers of a translation started from
        # a console or by another tool). `time` says how fresh it is; `finished` that the run is over.
        self.status_path, self.status_sent = None, 0.0

    def write_status(self, ok, failed, skipped, finished=False):
        if not self.status_path:
            return
        rate, left = self.left()
        status = dict({"pid": os.getpid(), "time": time.time(), "started": self.t0, "done": self.k,
                       "total": self.total, "ok": ok, "failed": failed, "skipped": skipped, "cached": self.cached,
                       "rate": round(rate, 2), "eta": None if left is None else round(left), "finished": finished},
                      **self.extra)
        try:
            part = self.status_path + ".part"
            with open(part, "w", encoding="utf-8") as f:
                json.dump(status, f)
            os.replace(part, self.status_path)
        except OSError:
            pass

    def left(self):
        dt = time.time() - self.t0
        rate = self.built / dt if dt > 0 and self.built else 0
        return rate, ((self.total - self.k) / rate if rate else None)

    def line(self, ok, failed):
        pct = 100 * self.k / max(1, self.total)
        rate, left = self.left()
        left = left or 0
        bar = "#" * int(pct / 4) + "-" * (25 - int(pct / 4))
        return (f"[{bar}] {pct:5.1f}%  {self.k}/{self.total}  DXIL {ok}  falhas {failed}  "
                f"{rate:.1f}/s  restante {int(left // 3600)}h{int(left % 3600 // 60):02d}m")

    def step(self, ok, failed, skipped=0, built=True, cached=False, force=False):
        with self.lock:
            if not force:
                self.k += 1
                self.built += built
                self.cached += cached
            pct = 100 * self.k // max(1, self.total)
            self.counts = (ok, failed, skipped)
            if force or time.time() - self.status_sent >= 1.0 or self.k == self.total:
                self.status_sent = time.time()
                self.write_status(ok, failed, skipped)
            if self.ui:
                now = time.time()
                if force or now - self.sent >= 0.5 or self.k == self.total:
                    self.sent = now
                    rate, left = self.left()
                    print("@@ui " + json.dumps(dict({"event": "progress", "stage": "shaders", "done": self.k,
                                                     "total": self.total, "ok": ok, "failed": failed,
                                                     "skipped": skipped, "cached": self.cached,
                                                     "eta": None if left is None else round(left)},
                                                    **self.extra)), flush=True)
            elif self.live:
                print("\r" + self.line(ok, failed), end="", flush=True)
                if os.name == "nt" and pct != self.last:
                    import ctypes
                    ctypes.windll.kernel32.SetConsoleTitleW(f"{pct}% shaders - Flamingo Port")
            elif pct != self.last:
                print(self.line(ok, failed), flush=True)
            self.last = pct

    def done(self):
        self.write_status(*getattr(self, "counts", (0, 0, 0)), finished=True)
        if self.live:
            print()


# ---------------------------------------------------------------- the build
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("disasm")
    ap.add_argument("dxbc")
    ap.add_argument("out")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--limit", type=int)
    ap.add_argument("--timeout", type=int, default=900,
                    help="seconds DXC may spend on one shader (the largest take minutes even at -O0)")
    ap.add_argument("--opt", choices=["0", "1", "3"], default="0", help="DXC optimisation level")
    ap.add_argument("--serve", type=int, default=int(os.environ.get("FLAMINGO_SERVE") or 0),
                    help="also hand HLSL to helper PCs on this port (plain HTTP, local network only)")
    ap.add_argument("--token", default=os.environ.get("FLAMINGO_TOKEN", ""), help="the helpers' password")
    ap.add_argument("--keep", help="keep the intermediates (SPIR-V, HLSL) in this folder")
    ap.add_argument("--defer-big", action="store_true",
                    help="leave shaders with more than --big-kb of disassembly for a later run (they take minutes "
                         "each in DXC; nearly all are ray-tracing compute shaders)")
    ap.add_argument("--big-kb", type=int, default=2500)
    ap.add_argument("--local-dxc", type=int, default=int(os.environ.get("FLAMINGO_LOCAL_DXC", -1)),
                    help="DXC processes on this machine (0: helpers compile, this machine only translates; it still "
                         "compiles what a helper failed, and takes the queue when no helper is seen for a minute); "
                         "default --jobs")
    ap.add_argument("--sidecars-only", action="store_true",
                    help="only write the vertex shaders' <digest>.vsin files, translate nothing")
    a = ap.parse_args()
    opt = "-O" + a.opt
    os.makedirs(a.out, exist_ok=True)
    if a.keep:
        os.makedirs(a.keep, exist_ok=True)
    files = sorted(os.path.join(a.disasm, f) for f in os.listdir(a.disasm) if f.endswith(".s"))
    if a.limit:
        files = files[: a.limit]

    n = len(files)
    progress = Progress(n)
    if not a.sidecars_only:
        progress.status_path = os.path.join(a.out, "translation.json")
        progress.extra["port"] = a.serve
    index, why = {}, Counter()
    lock = threading.Lock()
    queue = Queue(lease=a.timeout * 2 + 60)

    def counts():
        skipped = why["stage not handled"]
        progress.extra["deferred"] = why["deferred"]
        # Not failures: compute shaders the game hasn't made a pipeline of yet (their registers come from it, see
        # rdna2spirv.compute_regs). They are translated by the run after the game meets them.
        unmet = progress.extra["unmet"] = why["  translate: compute registers unknown"]
        return len(index), sum(v for k, v in why.items() if not k.startswith("  ")) - skipped - why["deferred"] - \
            unmet, skipped

    def record(name, entry, error, built=True, cached=False):
        with lock:
            if entry:
                index[entry["digest"]] = {"name": name, "stage": entry["stage"], "bytes": entry["bytes"]}
            else:
                why[error.split(":")[0] if ":" in error else error] += 1
                why["  " + error] += 1
            ok, failed, skipped = counts()
        progress.step(ok, failed, skipped, built=built, cached=cached)

    def finish(job, dxil, error):
        """A compiled (or failed) job, from here or from a helper."""
        d = job["digest"]
        if dxil is not None:
            part = os.path.join(a.out, f"{d}.dxil.{os.getpid()}.{threading.get_ident()}.part")
            with open(part, "wb") as f:
                f.write(dxil)
            os.replace(part, os.path.join(a.out, d + ".dxil"))
            with queue.cv:
                queue.by_machine[job.get("machine", "")] += 1
            record(job["name"], {"digest": d, "stage": job["stage"], "bytes": len(dxil)}, None)
        else:
            try:
                with open(os.path.join(a.out, d + ".fail"), "w", encoding="utf-8") as f:
                    f.write(f"{TOOLS_ID}\n{error}")
            except OSError:
                pass
            record(job["name"], None, error)

    # what's done already, and what's left: stage and digest are cheap, read here
    todo = []
    for s_path in files:
        name = os.path.splitext(os.path.basename(s_path))[0]
        stage = rdna2spirv.stage_of(name, s_path)
        if stage not in PROFILES:
            record(name, None, f"stage not handled: {stage}", built=False)
            continue
        digest = hash_digest(os.path.join(a.dxbc, name + ".dxbc"))
        if not digest:
            record(name, None, "no HASH digest", built=False)
            continue
        if stage == "vs":
            write_vsin(a.out, digest, os.path.join(a.dxbc, name + ".dxbc"))
        if a.sidecars_only:
            continue
        dxil = os.path.join(a.out, digest + ".dxil")
        if os.path.isfile(dxil):
            record(name, {"digest": digest, "stage": stage, "bytes": os.path.getsize(dxil)}, None, built=False,
                   cached=True)
            continue
        reason = kept_failure(os.path.join(a.out, digest + ".fail"))
        if reason and "compute registers unknown" in reason and                 os.path.isfile(os.path.join(a.out, "cs", digest + ".regs")):
            reason = None  # the runtime has seen its pipeline since: its registers are known now
        if reason:
            record(name, None, reason, built=False)
            continue
        size = os.path.getsize(s_path)
        if a.defer_big and size > a.big_kb * 1024:
            record(name, None, "deferred", built=False)
            continue
        todo.append({"path": s_path, "name": name, "stage": stage, "digest": digest, "size": size})
    todo.sort(key=lambda j: j["size"])      # small first: the port is usable long before the largest are done
    if a.sidecars_only:
        progress.done()
        print("vertex-shader input files written")
        return 0

    httpd = None
    if a.serve and todo:
        if not a.token:
            print("--serve needs a --token (FLAMINGO_TOKEN)", file=sys.stderr)
            return 2
        httpd = serve(queue, a.serve, a.token, opt, a.timeout, finish)
        print(f"helpers welcome on port {a.serve} (local network)", flush=True)

    # local compilers: threads (DXC runs as its own process)
    translating = [0]

    local_dxc = a.jobs if a.local_dxc < 0 else a.local_dxc
    helpers_seen = [time.time()]

    def compiler():
        while True:
            only_retries = False
            if local_dxc == 0:
                if queue.active_helpers():
                    helpers_seen[0] = time.time()
                only_retries = time.time() - helpers_seen[0] < 60
            job = queue.take(1.0, local_only=only_retries)
            if job is None:
                if queue.over:
                    return
                continue
            dxil, error = run_dxc(job["hlsl"], PROFILES[job["stage"]], opt, a.timeout)
            finish(job, dxil, error)

    threads = [threading.Thread(target=compiler, daemon=True) for _ in range(max(1, local_dxc) if local_dxc else 2)]
    if local_dxc == 0:
        print("this machine translates; helpers compile", flush=True)
    for t in threads:
        t.start()

    # translation feeds the queue, kept a little ahead of the compilers (HLSL is held in memory)
    ahead = max(16, 4 * a.jobs)
    with ProcessPoolExecutor(a.jobs, initializer=init, initargs=(os.path.join(a.out, "cs"),)) as pool:
        pending = {}
        it = iter(todo)
        exhausted = False
        last_helpers = None
        while True:
            while not exhausted and len(pending) + queue.size() < ahead + 2 * len(queue.active_helpers()) * 8:
                job = next(it, None)
                if job is None:
                    exhausted = True
                    break
                pending[pool.submit(to_hlsl, job["path"], a.keep)] = job
            done = [f for f in pending if f.done()]
            for f in done:
                job = pending.pop(f)
                hlsl, error = f.result()
                if hlsl is None:
                    finish(job, None, error)
                else:
                    job["hlsl"] = hlsl
                    queue.put(job)
            helpers = queue.active_helpers()
            if helpers != last_helpers:
                last_helpers = helpers
                progress.extra["helpers"] = helpers
                if helpers:
                    print("helpers: " + ", ".join(helpers), flush=True)
            progress.extra["by_helpers"] = queue.by_helpers
            with queue.cv:
                progress.extra["machines"] = dict(queue.by_machine)
            progress.extra["working"], progress.extra["waiting"] = queue.working()
            if time.time() - progress.status_sent >= 1.0:   # the status file stays fresh while nothing finishes
                progress.status_sent = time.time()
                progress.write_status(*getattr(progress, "counts", (0, 0, 0)))
            if exhausted and not pending and queue.size() == 0:
                break
            if not done:
                time.sleep(0.05)
    queue.over = True
    with queue.cv:
        queue.cv.notify_all()
    for t in threads:
        t.join()
    if httpd:
        time.sleep(1)                       # helpers waiting on a job hear "over"
        httpd.shutdown()
    progress.done()
    with open(os.path.join(a.out, "index.json"), "w") as f:
        json.dump(index, f, indent=0)
    print(f"{n} shaders: {len(index)} in DXIL" + (f" ({queue.by_helpers} by helpers)" if queue.by_helpers else ""))
    for k, c in why.most_common(30):
        print(f"  {c:6}  {k}")


if __name__ == "__main__":
    sys.exit(main())
