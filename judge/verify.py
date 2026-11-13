#!/usr/bin/env python3
"""verify.py - Problem 4 (Cat-V Minimalist Stack) harness. Run via verify.sh.

  verify.sh [--config verify.yaml] [--runs N] [--no-perf] [--keep] [--verbose]

Boots your image with judge/qemu.sh, offline, and checks:
  L0  boot to a shell and answer a split-nonce command within 120 s (gate,
      up to 3 attempts)                                          required
  L1  supervised HEARTBEAT service restarts with a new pid after a kill,
      and poweroff makes QEMU exit within 15 s                   6 points
  L2  osprobe: proc, pipe, signal, mmap, thread, clock           10 points
  L3  persistence across a poweroff and a second boot            7 points
  L4  SMP: two busy workers take at most 1.3x one worker         7 points
and, unless --no-perf, the three tuning measurements (QEMU RSS after 30 s
idle at the prompt, xz -9e image size, launch-to-nonce time).

Every timing is taken on the host. Your verify.yaml strings are only ever
sent to the guest console (or, for osprobe.host_build, run as an argument
list); nothing goes through a host shell.
"""
import argparse, io, json, os, random, re, select, shutil, signal, subprocess, sys, tarfile, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
M64 = (1 << 64) - 1

# ---------------------------------------------------------- osprobe mirror
def mix(x):
    x = (x + 0x9E3779B97F4A7C15) & M64
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & M64
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & M64
    return x ^ (x >> 31)

def token(seed, check):
    return mix(seed ^ ((check + 1) << 40))

def stream(seed, n):
    x = seed | 1
    out = bytearray(n)
    for i in range(0, n, 8):
        x ^= x >> 12; x ^= (x << 25) & M64; x ^= x >> 27
        v = (x * 0x2545F4914F6CDD1D) & M64
        out[i:i + 8] = v.to_bytes(8, "little")[: n - i]
    return bytes(out)

def fnv(b):
    h = 0xcbf29ce484222325
    for c in b:
        h = ((h ^ c) * 0x100000001b3) & M64
    return h

def expected(seed):
    e = {"proc": mix(token(seed, 0)), "pipe": fnv(stream(token(seed, 1), 65536))}
    t = token(seed, 2); acc = t
    for i in range(1, 3 + t % 5 + 1):
        acc = mix((acc + i) & M64)
    e["signal"] = acc
    t = token(seed, 3); a = stream(t, 1 << 20); h = fnv(a)
    e["mmap"] = (h + 3 * h + mix((t + 1) & M64)) & M64
    b = stream(token(seed, 4), 1 << 18); q = len(b) // 4
    e["thread"] = sum(fnv(b[i * q:(i + 1) * q]) * (i + 1) for i in range(4)) & M64
    e["clock"] = mix(token(seed, 5))
    return e

CHECKS = ["proc", "pipe", "signal", "mmap", "thread", "clock"]

# ----------------------------------------------------------------- config
def load_config(path):
    import yaml
    with open(path) as f:
        c = yaml.safe_load(f) or {}
    base = os.path.dirname(os.path.abspath(path))
    def p(x):
        return os.path.join(base, x) if x and not os.path.isabs(x) else x
    boot = c.get("boot", {}) or {}
    for k in ("system_image", "kernel", "initrd"):
        boot[k] = p(boot.get(k))
    c["boot"] = boot
    c.setdefault("osprobe", {})
    c.setdefault("data", {})
    c["_base"] = base
    for k in ("prompt_regex", "nonce_command", "kill_command", "poweroff_command"):
        if not c.get(k):
            sys.exit("verify.yaml: missing %s" % k)
    return c

def as_list(x):
    if x is None: return []
    return x if isinstance(x, list) else [x]

# ------------------------------------------------------------------ guest
class Guest:
    def __init__(self, cfg, disks, log, verbose):
        b = cfg["boot"]
        args = [os.path.join(HERE, "qemu.sh"), disks["system"], disks["assets"], disks["data"]]
        if b.get("kernel"):
            args += [b["kernel"], b.get("initrd") or "", b.get("append") or ""]
        self.cfg, self.verbose = cfg, verbose
        self.log = open(log, "ab")
        self.log.write(("\n### launch %s\n" % " ".join(args)).encode())
        self.t0 = time.time()
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  start_new_session=True)
        self.buf = ""          # everything received
        self.lines = []        # (time, line)
        self.partial = ""
        self.heartbeats = []   # (time, pid, n)

    def pump(self, timeout):
        r, _, _ = select.select([self.p.stdout], [], [], max(0.0, timeout))
        if not r:
            return False
        data = os.read(self.p.stdout.fileno(), 65536)
        if not data:
            return None
        self.log.write(data); self.log.flush()
        s = data.decode(errors="replace").replace("\r", "")
        if self.verbose:
            sys.stdout.write(s); sys.stdout.flush()
        self.buf += s
        self.partial += s
        now = time.time()
        while "\n" in self.partial:
            ln, self.partial = self.partial.split("\n", 1)
            self.lines.append((now, ln))
            m = re.search(r"HEARTBEAT (\d+) (\d+)", ln)
            if m:
                self.heartbeats.append((now, int(m.group(1)), int(m.group(2))))
        return True

    def wait_for(self, pred, timeout):
        end = time.time() + timeout
        while True:
            v = pred()
            if v: return v
            left = end - time.time()
            if left <= 0 or self.p.poll() is not None and not self.pump(0):
                return pred()
            if self.pump(min(left, 0.5)) is None:
                return pred()

    def send(self, line):
        self.log.write(("\n### send: %s\n" % line).encode())
        try:
            self.p.stdin.write((line + "\n").encode()); self.p.stdin.flush()
        except BrokenPipeError:
            pass

    def split_cmd(self, text):
        a, b = text[: len(text) // 2], text[len(text) // 2:]
        return self.cfg["nonce_command"].replace("{a}", a).replace("{b}", b)

    def seen_line(self, text, since):
        return any(t >= since and text in l and self.split_cmd(text) not in l for t, l in self.lines)

    def run(self, cmd, timeout):
        """Send cmd, then a split marker; wait for the joined marker.
        Returns the output lines in between, or None on timeout."""
        marker = "M%06x" % random.getrandbits(24) + "Z"
        start = time.time()
        n0 = len(self.lines)
        self.send(cmd)
        self.send(self.split_cmd(marker))
        ok = self.wait_for(lambda: self.seen_line(marker, start), timeout)
        if not ok:
            return None
        out = []
        for t, l in self.lines[n0:]:
            if marker in l and self.split_cmd(marker) not in l:
                break
            out.append(l)
        return out

    def rss_kib(self):
        try:
            kids = subprocess.run(["pgrep", "-f", "qemu-system-x86_64", "-s", str(self.p.pid)],
                                  capture_output=True, text=True).stdout.split()
            pid = int(kids[0]) if kids else self.p.pid
            for l in open("/proc/%d/status" % pid):
                if l.startswith("VmRSS:"):
                    return int(l.split()[1])
        except Exception:
            return None

    def exited_within(self, timeout):
        try:
            self.p.wait(timeout=timeout)
            return True
        except subprocess.TimeoutExpired:
            return False

    def kill(self):
        if self.p.poll() is None:
            try:
                os.killpg(self.p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.p.wait()
        self.log.close()

# ------------------------------------------------------------------- disks
def make_assets(cfg, work, seed):
    d = os.path.join(work, "assets"); os.makedirs(d, exist_ok=True)
    src = open(os.path.join(HERE, "osprobe.c")).read()
    src, n = re.subn(r"#define OSPROBE_SEED 0x[0-9a-fA-F]+ULL", "#define OSPROBE_SEED 0x%016xULL" % seed, src)
    if n != 1:
        sys.exit("osprobe.c: seed line not found")
    open(os.path.join(d, "osprobe.c"), "w").write(src)
    hb = cfg["osprobe"].get("host_build")
    if hb:
        argv = [x.replace("{src}", os.path.join(d, "osprobe.c")).replace("{out}", os.path.join(d, "osprobe")) for x in hb]
        r = subprocess.run(argv, capture_output=True, text=True, cwd=cfg["_base"])
        if r.returncode:
            print("  osprobe host_build failed:\n" + r.stderr)
    bio = io.BytesIO()
    with tarfile.open(fileobj=bio, mode="w", format=tarfile.USTAR_FORMAT) as tf:
        for fn in sorted(os.listdir(d)):
            ti = tf.gettarinfo(os.path.join(d, fn), arcname=fn)
            ti.uid = ti.gid = 0; ti.uname = ti.gname = "root"
            with open(os.path.join(d, fn), "rb") as fh:
                tf.addfile(ti, fh)
    data = bio.getvalue()
    data += b"\0" * ((-len(data)) % (1 << 20) or 0)
    img = os.path.join(work, "assets.img")
    open(img, "wb").write(data)
    return img

def make_blank(path, mib):
    with open(path, "wb") as f:
        f.truncate(mib << 20)

# -------------------------------------------------------------------- run
class Run:
    def __init__(self, cfg, work, args, idx):
        self.cfg, self.work, self.args, self.idx = cfg, work, args, idx
        self.res, self.notes, self.perf = {}, [], {}
        self.log = os.path.join(work, "run%02d.log" % idx)

    def note(self, s):
        self.notes.append(s)
        print("    " + s)

    def boot_to_prompt(self, disks, label):
        """L0 with up to 3 attempts. Returns (guest, seconds_to_nonce) or (None, None)."""
        for attempt in range(1, 4):
            g = Guest(self.cfg, disks, self.log, self.args.verbose)
            rx = re.compile(self.cfg["prompt_regex"], re.M)
            got = g.wait_for(lambda: rx.search(g.buf[-400:]) is not None, 120)
            if got:
                nonce = "%08x%08x" % (random.getrandbits(32), random.getrandbits(32))
                t = time.time()
                g.send(g.split_cmd(nonce))
                left = 120 - (time.time() - g.t0)
                if g.wait_for(lambda: g.seen_line(nonce, t), max(left, 1)):
                    secs = next(tt for tt, l in g.lines if nonce in l and g.split_cmd(nonce) not in l) - g.t0
                    self.note("%s: shell answered the split nonce after %.1f s (attempt %d)" % (label, secs, attempt))
                    return g, secs
                self.note("%s: prompt seen but the nonce was not answered in time (attempt %d)" % (label, attempt))
            elif g.p.poll() is not None and time.time() - g.t0 < 120:
                tail = g.buf.strip().splitlines()[-3:]
                self.note("%s: QEMU exited after %.1f s (attempt %d): %s" % (label, time.time() - g.t0, attempt, " | ".join(tail)))
            else:
                self.note("%s: no prompt matching %r within 120 s (attempt %d)" % (label, self.cfg["prompt_regex"], attempt))
            g.kill()
        return None, None

    def setup_osprobe(self, g):
        o = self.cfg["osprobe"]
        for c in as_list(o.get("setup")) + as_list(o.get("build")):
            if g.run(c, 600) is None:
                self.note("osprobe setup/build command timed out: %s" % c)
                return False
        return True

    def l1(self, g):
        hb = g.wait_for(lambda: len(g.heartbeats) >= 2 and g.heartbeats[-1][2] > g.heartbeats[-2][2] - 0 and g.heartbeats, 15)
        if not hb:
            self.note("L1: no HEARTBEAT <pid> <n> lines from the supervised service")
            return False
        _, pid, n = g.heartbeats[-1]
        t = time.time()
        g.send(self.cfg["kill_command"].replace("{pid}", str(pid)))
        new = g.wait_for(lambda: [h for h in g.heartbeats if h[0] >= t and h[1] != pid], 5.0 + 1.0)
        if not new:
            self.note("L1: after killing pid %d no heartbeat from a new pid within 5 s" % pid)
            return False
        tt, npid, nn = new[0]
        if nn != 1:
            self.note("L1: new pid %d started counting at %d, expected 1" % (npid, nn))
            return False
        if tt - t > 5.0:
            self.note("L1: restart took %.1f s (limit 5 s)" % (tt - t))
            return False
        self.note("L1: killed pid %d, pid %d restarted the count after %.2f s" % (pid, npid, tt - t))
        return True

    def l2(self, g, seed):
        if self.cfg["osprobe"].get("native"):
            self.note("L2: native probe declared; reviewed by the judges with source and a live run")
            return None
        if not self.setup_osprobe(g):
            return {c: False for c in CHECKS}
        out = g.run(self.cfg["osprobe"].get("run", "/tmp/osprobe/osprobe {check}").replace("{check}", "all"), 300)
        exp = expected(seed)
        got = {}
        for l in out or []:
            m = re.search(r"PASS (\w+) ([0-9a-f]{16})", l)
            if m: got[m.group(1)] = int(m.group(2), 16)
            m = re.search(r"FAIL (\w+) (.*)", l)
            if m: self.note("L2: osprobe reported FAIL %s %s" % (m.group(1), m.group(2)))
        res = {}
        for c in CHECKS:
            res[c] = got.get(c) == exp[c]
            if c in got and not res[c]:
                self.note("L2: %s printed %016x, expected %016x" % (c, got[c], exp[c]))
            elif c not in got:
                self.note("L2: %s: no PASS line" % c)
        return res

    def marks_since(self, g, n0, names):
        marks = {}
        for t, l in g.lines[n0:]:
            for m in names:
                if l.strip().endswith(m) and m not in marks:
                    marks[m] = t
        return marks

    def l4(self, g):
        run = self.cfg["osprobe"].get("run", "/tmp/osprobe/osprobe {check}")
        # Size the work on the HOST clock: about 3 s for one worker.
        iters, secs = 20000000, 0.0
        for _ in range(8):
            n0 = len(g.lines)
            if g.run(run.replace("{check}", "burn %d" % iters), 600) is None:
                self.note("L4: osprobe burn did not finish"); return False
            mk = self.marks_since(g, n0, ("BURN-BEGIN", "BURN-END"))
            if len(mk) < 2:
                self.note("L4: BURN markers missing"); return False
            secs = mk["BURN-END"] - mk["BURN-BEGIN"]
            if secs >= 0.5: break
            iters *= 4
        iters = int(iters * 3.0 / max(secs, 1e-3))
        n0 = len(g.lines)
        out = g.run(run.replace("{check}", "smp %d" % iters), 900)
        marks = {}
        for t, l in g.lines[n0:]:
            for m in ("SMP-ONE-BEGIN", "SMP-ONE-END", "SMP-TWO-BEGIN", "SMP-TWO-END"):
                if l.strip().endswith(m):
                    marks[m] = t
        if out is None or len(marks) < 4:
            self.note("L4: SMP markers missing")
            return False
        one = marks["SMP-ONE-END"] - marks["SMP-ONE-BEGIN"]
        two = marks["SMP-TWO-END"] - marks["SMP-TWO-BEGIN"]
        ratio = two / one if one > 0 else 99
        self.note("L4: one worker %.2f s, two workers %.2f s (ratio %.2f, limit 1.30)" % (one, two, ratio))
        self.perf["smp_ratio"] = ratio
        return ratio <= 1.3

    def execute(self):
        cfg, w = self.cfg, self.work
        seed = random.getrandbits(64)
        sysimg = cfg["boot"].get("system_image")
        if not sysimg:
            sysimg = os.path.join(w, "empty-system.img"); make_blank(sysimg, 1)
        disks = {"system": sysimg, "assets": make_assets(cfg, w, seed), "data": os.path.join(w, "data.img")}
        make_blank(disks["data"], int(cfg["data"].get("size_mib", 64)))
        print("  run %d (osprobe seed %016x, log %s)" % (self.idx, seed, self.log))

        # ---- boot 1
        g, secs = self.boot_to_prompt(disks, "boot 1")
        self.res["L0"] = g is not None
        if not g:
            return self.res
        self.perf["boot_to_nonce_s"] = secs
        if not self.args.no_perf:
            prompt_t = time.time()
            g.wait_for(lambda: False, max(0, 30 - (time.time() - prompt_t)))
            self.perf["idle_rss_kib"] = g.rss_kib()
            self.note("perf: QEMU RSS after 30 s idle: %s KiB" % self.perf["idle_rss_kib"])
        l1_restart = self.l1(g)
        self.res["L2"] = self.l2(g, seed)
        nonce3 = "%016x" % random.getrandbits(64)
        l3_written = True
        for c in as_list(cfg["data"].get("setup")):
            if g.run(c, 300) is None: l3_written = False
        if g.run(cfg["data"].get("write", "echo {nonce} > /mnt/data/nonce").replace("{nonce}", nonce3), 120) is None:
            l3_written = False
        t = time.time()
        g.send(cfg["poweroff_command"])
        off = g.exited_within(15)
        self.note("L1: poweroff %s (%.1f s)" % ("made QEMU exit" if off else "did NOT make QEMU exit within 15 s", time.time() - t))
        g.kill()
        self.res["L1"] = l1_restart and off
        self.res["L1_parts"] = {"restart": l1_restart, "poweroff": off}

        # ---- boot 2
        g, _ = self.boot_to_prompt(disks, "boot 2")
        if not g:
            self.res["L3"] = False; self.res["L4"] = False
            return self.res
        for c in as_list(cfg["data"].get("setup")):
            g.run(c, 300)
        out = g.run(cfg["data"].get("read", "cat /mnt/data/nonce"), 120) or []
        self.res["L3"] = l3_written and any(nonce3 in l for l in out)
        self.note("L3: value %s after reboot" % ("read back" if self.res["L3"] else "NOT read back"))
        if not cfg["osprobe"].get("native"):
            self.setup_osprobe(g)
            self.res["L4"] = self.l4(g)
        else:
            self.res["L4"] = None
        g.send(cfg["poweroff_command"])
        g.exited_within(15)
        g.kill()
        return self.res

def score(res):
    if not res.get("L0"):
        return 0.0
    s = 0.0
    s += 6 if res.get("L1") else 0
    l2 = res.get("L2")
    if isinstance(l2, dict):
        s += 10 * sum(1 for v in l2.values() if v) / len(CHECKS)
    s += 7 if res.get("L3") else 0
    s += 7 if res.get("L4") else 0
    return s

def image_size(cfg):
    files = [cfg["boot"].get(k) for k in ("system_image", "kernel", "initrd") if cfg["boot"].get(k)]
    total = 0
    for f in files:
        r = subprocess.run(["xz", "-9e", "-T1", "-c", f], stdout=subprocess.PIPE)
        total += len(r.stdout)
    return total

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default="verify.yaml")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--no-perf", action="store_true")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--verbose", action="store_true", help="echo the guest console")
    ap.add_argument("--json", help="write results to this file")
    args = ap.parse_args()
    if shutil.which("qemu-system-x86_64") is None:
        sys.exit("qemu-system-x86_64 not found")
    cfg = load_config(args.config)
    work = tempfile.mkdtemp(prefix="catv-verify-")
    print("Cat-V verify: %s, %d run(s), work dir %s" % (args.config, args.runs, work))
    runs = []
    for i in range(1, args.runs + 1):
        r = Run(cfg, work, args, i)
        res = r.execute()
        runs.append({"result": res, "perf": r.perf, "notes": r.notes, "score": score(res)})
        def word(k):
            if k not in res: return "-"
            v = res[k]
            if k == "L2" and isinstance(v, dict): return "%d/6" % sum(1 for x in v.values() if x)
            return "review" if v is None else ("PASS" if v else "FAIL")
        print("  run %d: L0 %s  L1 %s  L2 %s  L3 %s  L4 %s  -> correctness %.1f / 30" % (
            i, word("L0"), word("L1"), word("L2"), word("L3"), word("L4"), score(res)))
    full = sum(1 for r in runs if r["score"] >= 30 - 1e-9)
    if not args.no_perf and runs and runs[0]["result"].get("L0"):
        sz = image_size(cfg)
        print("  perf: xz -9e image size %d bytes (%.1f KiB)" % (sz, sz / 1024))
        runs[0]["perf"]["xz_bytes"] = sz
    print("\nsummary: %d of %d run(s) fully passing%s" % (full, len(runs),
          "  (Robustness needs --runs 10 to pass 10 of 10)" if args.runs != 10 else ""))
    if args.json:
        json.dump(runs, open(args.json, "w"), indent=2, default=str)
    if not args.keep:
        for fn in os.listdir(work):
            if fn.endswith(".img"):
                os.unlink(os.path.join(work, fn))
    print("logs: %s" % work)
    return 0 if full == len(runs) else 1

if __name__ == "__main__":
    sys.exit(main())
