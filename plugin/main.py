"""
FSR4 NPU — Decky backend: all-in-one installer + live knobs for the
FSR4-on-NPU No Man's Sky port.

Distributable payload (bundled in payload/, plugin code):
  libxess.dll                   the seat (XeSS-slot takeover DLL)
  fsr4npu_seam.aarch64.so       the Vulkan implicit layer
  fsr4npu_seam.json             the layer manifest
  q6bench                       benchmark harness — installed only when
                                present and checksum-valid; the other
                                three payload files always ship
Licensed (AMD/QNN — fetched from a user-supplied download URL, verified by
checksum after download):
  bin_cfnhwc{,_native,_quality,_balanced,_perf,_ultraperf}.bin   NPU contexts
  pass0_{native,quality,balanced,perf,ultraperf,wb}.raw          tier weights
checksums.json (bundled) carries md5s for both sets. The /etc/armada/
game-tweaks.json env merge is done directly (the plugin runs with the root
flag); a .bak is kept next to every privileged write.
"""
import asyncio
import glob
import hashlib
import json
import os
import re
import shutil
import tarfile
import time
import urllib.request
import zipfile

import decky_plugin

PLUGIN_DIR = os.path.dirname(os.path.abspath(__file__))
PAYLOAD = os.path.join(PLUGIN_DIR, "payload")
CFG_DIR = decky_plugin.DECKY_PLUGIN_SETTINGS_DIR

KNOB_DIR = "/var/home/armada/fsr4npu-logs"
LAYER_DIR = "/var/home/armada/.local/share/vulkan/implicit_layer.d"
GAME_BIN = ("/var/home/armada/.local/share/Steam/steamapps/common/"
            "No Man's Sky/Binaries")
QNN_DIR = "/var/home/armada/.local/share/fsr4npu/qnn"
TWEAKS = "/etc/armada/game-tweaks.json"

NUMERIC = {
    "BFLOOR": "0", "BFTAU": "0.35", "EXPO": "4.0",
    "SHARP": "0.5", "DILATE": "0", "VCORNER": "0xA0",
}
MARKERS = ("AUTOEXPO", "W3", "ZCC2", "ZCC3", "PDISOC")
# defaults the installer enables; W3 gates NPU execution and ZCC2/ZCC3 gate
# zero-copy import
INSTALL_MARKERS = [m for m in MARKERS if m != "PDISOC"]
# marker locations: AUTOEXPO + DUMP are read from the game's Binaries/ by the
# seat; W3/ZCC2/ZCC3/PDISOC are read by the layer from ~/fsr4npu-logs/ — a
# marker in the wrong dir silently does nothing.
GAME_BIN_MARKERS = ("AUTOEXPO", "DUMP")


def marker_path(name):
    d = GAME_BIN if name in GAME_BIN_MARKERS else KNOB_DIR
    return os.path.join(d, "fsr4npu-" + name)


def knob_path(name):
    return os.path.join(KNOB_DIR, "fsr4npu-" + name)


URL_FILES = [
    "bin_cfnhwc.bin", "bin_cfnhwc_native.bin", "bin_cfnhwc_quality.bin",
    "bin_cfnhwc_balanced.bin", "bin_cfnhwc_perf.bin", "bin_cfnhwc_ultraperf.bin",
    "pass0_native.raw", "pass0_quality.raw", "pass0_balanced.raw",
    "pass0_perf.raw", "pass0_ultraperf.raw", "pass0_wb.raw",
]
# per-set counts for status/report: 6 NPU context bins + 6 tier weights
URL_BINS = sum(1 for n in URL_FILES if n.endswith(".bin"))
URL_PASS0 = len(URL_FILES) - URL_BINS


def _runtime_files(arch):
    """arch-suffixed where it matters (stub/skel — capital V, e.g.
    libQnnHtpV73Stub.so); the rest are arch-neutral. An empty arch falls
    back to v73 names (this build's shipped pack)."""
    a = (arch or "v73").lstrip("vV")
    return [
        "libQnnHtp.so",
        f"libQnnHtpV{a}Stub.so",
        f"libQnnHtpV{a}Skel.so",
        "libQnnSystem.so", "libcdsprpc.so", "libc++.so.1", "libc++abi.so.1",
        "fastrpc_shell_unsigned_3",
    ]


# the Qualcomm runtime the layer dlopens at probe time (4 QNN libs + fastRPC +
# C++ runtime + the unsigned-PD shell); libatomic.so.1 comes from the OS.
RUNTIME_FILES = _runtime_files(None)
URL_SETS = {"weights": URL_FILES, "runtime": RUNTIME_FILES}
ARCHIVE_EXTS = (".tar.gz", ".tgz", ".tar", ".zip")


def _dst_for(which, name):
    """where a licensed file belongs: bins+runtime -> QNN_DIR, pass0s -> Binaries"""
    if which == "runtime" or name.endswith(".bin"):
        return os.path.join(QNN_DIR, name)
    return os.path.join(GAME_BIN, name)


TWEAK_ENV = {
    "ENABLE_FSR4NPU_SEAM": "1",
    "FSR4NPU_NPU": "1",
    "FSR4NPU_SPOOF": "0",
    "FSR4NPU_SPOOF_FIELDS": "63",
    "FSR4NPU_SPOOF_REVERT_MS": "1500",
    "SL_LOG_LEVEL": "2",
    # must stay the wine (Z:\...) form of KNOB_DIR
    "SL_LOG_PATH": "Z:\\var\\home\\armada\\fsr4npu-logs",
    "WINEDLLOVERRIDES": "nvngx=n,b;vulkan-1=b,n;nvapi64=n,b;combase=n,b;"
                        "wintrust=n,b;version=n,b",
    "DISABLE_VK_LAYER_VALVE_steam_fossilize_1": "1",
}
# legacy env var, also cleared on install/uninstall
SCRUB_ENV = list(TWEAK_ENV) + ["FSR4NPU_VKTRACE"]
# the same DLL overrides also written into the game's wine prefix registry:
# Steam launch options are user-editable and do get cleared; the registry
# copy makes the overrides survive that.
WINE_DLL_OVERRIDES = {
    "nvngx": "native,builtin", "vulkan-1": "builtin,native",
    "nvapi64": "native,builtin", "combase": "native,builtin",
    "wintrust": "native,builtin", "version": "native,builtin",
}
PREFIX_PFX = ("/var/home/armada/.local/share/Steam/steamapps/compatdata/"
              "275850/pfx/user.reg")
GAMES = [{"id": "nms", "name": "No Man's Sky"}]


def _write_wine_overrides():
    """Merge the plugin's DllOverrides into the game prefix's user.reg
    (created on the game's first run; absent until then). Returns
    (state, detail): state is "ok", "missing" (prefix not created yet) or
    "error"; detail is the display text for the "error" state."""
    if not os.path.exists(PREFIX_PFX):
        return "missing", None
    try:
        with open(PREFIX_PFX, encoding="utf-8", errors="replace") as f:
            reg = f.read()
    except OSError as e:
        return "error", f"prefix read: {e}"
    header = "[Software\\\\Wine\\\\DllOverrides]"
    if header not in reg:
        block = "\n".join(
            f'"{k}"="{v}"' for k, v in sorted(WINE_DLL_OVERRIDES.items()))
        reg = reg.rstrip("\n") + f"\n\n{header}\n{block}\n"
    else:
        add = []
        for k, v in sorted(WINE_DLL_OVERRIDES.items()):
            line = f'"{k}"="{v}"'
            pat = re.compile(r'^"' + re.escape(k) + r'"="[^"]*"$', re.M)
            if pat.search(reg):
                reg = pat.sub(line, reg, count=1)
            else:
                add.append(line)
        if add:
            reg = reg.replace(header, header + "\n" + "\n".join(add), 1)
    try:
        if not os.path.exists(PREFIX_PFX + ".bak-fsr4npu"):
            shutil.copy2(PREFIX_PFX, PREFIX_PFX + ".bak-fsr4npu")
        with open(PREFIX_PFX, "w", encoding="utf-8") as f:
            f.write(reg)
    except OSError as e:
        return "error", f"prefix write: {e}"
    return "ok", None


def _wine_overrides_present():
    try:
        with open(PREFIX_PFX, encoding="utf-8", errors="replace") as f:
            reg = f.read()
        return all(f'"{k}"="{v}"' in reg for k, v in WINE_DLL_OVERRIDES.items())
    except OSError:
        return False


def _log(msg):
    decky_plugin.logger.info(msg)


def _md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _tail(path, nbytes=262144):
    with open(path, "rb") as f:
        f.seek(0, os.SEEK_END)
        f.seek(max(0, f.tell() - nbytes))
        return f.read().decode("utf-8", "replace")


# the shipped NPU context binaries are HTP v73; this table exists so the user
# can see whether their chip matches the shipped pack before obtaining the
# runtime. Confident machine->arch entries only; unknown chips are reported
# as unknown rather than guessed.
ARCH_BY_MACHINE = [
    (("8550", "x1e", "x1p"), "v73"),
    (("8650",), "v75"),
    (("8450", "8475"), "v69"),
    (("8350",), "v68"),
]


def _arch_for(machine, compat):
    low = (machine + " " + compat).lower()
    for pats, arch in ARCH_BY_MACHINE:
        if any(p in low for p in pats):
            return arch
    return ""


def _sum(sums, arch, name):
    """per-arch checksums live under "<arch>/<name>"; plain names are the
    v73 shipped pack. A non-v73 arch with no per-arch entry falls back to
    the plain checksum."""
    if arch and arch != "v73":
        return sums.get(arch + "/" + name, sums.get(name))
    return sums.get(name)


def _soc_info():
    def rd(p):
        try:
            with open(p) as f:
                return f.read().strip()
        except OSError:
            return ""
    machine = rd("/sys/devices/soc0/machine")
    soc_id = rd("/sys/devices/soc0/soc_id")
    family = rd("/sys/devices/soc0/family")
    revision = rd("/sys/devices/soc0/revision")
    model = (rd("/proc/device-tree/model") or
             rd("/sys/firmware/devicetree/base/model")).replace("\x00", "")
    compat = rd("/proc/device-tree/compatible").replace("\x00", " ")
    hexagon = _arch_for(machine, compat)
    # supported = this build ships v73 model files; needs-pack = known arch
    # whose model pack is not shipped; unknown = no confident match
    if hexagon == "v73":
        support = "supported"
    elif hexagon:
        support = "needs-pack"
    else:
        support = "unknown"
    return {"model": model, "machine": machine, "soc_id": soc_id,
            "family": family, "revision": revision, "hexagon": hexagon,
            "support": support, "match": hexagon == "v73"}


def _checks():
    with open(os.path.join(PAYLOAD, "checksums.json")) as f:
        sums = json.load(f)
    state = {"seat": False, "layer": False, "manifest": False,
             "bins": 0, "pass0": 0, "env": False, "game": os.path.isdir(GAME_BIN),
             "qnn": False}
    seat = os.path.join(GAME_BIN, "libxess.dll")
    if os.path.exists(seat) and _md5(seat) == sums.get("libxess.dll"):
        state["seat"] = True
    so = os.path.join(LAYER_DIR, "fsr4npu_seam.aarch64.so")
    if os.path.exists(so) and _md5(so) == sums.get("fsr4npu_seam.aarch64.so"):
        state["layer"] = True
    state["manifest"] = os.path.exists(os.path.join(LAYER_DIR, "fsr4npu_seam.json"))
    arch = _soc_info()["hexagon"]
    for n in URL_FILES:
        p = os.path.join(QNN_DIR, n) if n.endswith(".bin") else os.path.join(GAME_BIN, n)
        if os.path.exists(p) and _md5(p) == _sum(sums, arch, n):
            if n.endswith(".bin"):
                state["bins"] += 1
            else:
                state["pass0"] += 1
    rt_files = _runtime_files(arch)
    state["runtime"] = sum(
        1 for n in rt_files
        if os.path.exists(os.path.join(QNN_DIR, n))
        and _md5(os.path.join(QNN_DIR, n)) == _sum(sums, arch, n))
    state["qnn"] = state["runtime"] >= 5  # the libs without the shell/c++ pair
    state["prefix"] = _wine_overrides_present()
    # without the armada-game-launch wrapper in NMS's Steam launch options,
    # none of the tweaks env reaches the game
    lo = ""
    for p in glob.glob("/var/home/armada/.local/share/Steam/userdata/*/config/localconfig.vdf"):
        try:
            with open(p, encoding="utf-8", errors="replace") as f:
                m = re.search(r'"275850"\s*\{[^{}]*?"LaunchOptions"\s*"([^"]*)"', f.read())
        except OSError:
            continue
        if m:
            lo = m.group(1)
    state["launchopts"] = "armada-game-launch" in lo
    try:
        with open(TWEAKS) as f:
            env = json.load(f).get("games", {}).get("275850", {}).get("env", {})
        state["env"] = env.get("FSR4NPU_NPU") == "1"
    except OSError:
        pass
    state["installed"] = (state["seat"] and state["layer"] and state["manifest"]
                          and state["bins"] >= 1 and state["pass0"] >= 1 and state["env"])
    return state


def _read_knobs():
    """Current knob file contents (stripped), keyed by name; knobs with no
    file on disk are absent from the result."""
    out = {}
    for name in NUMERIC:
        try:
            with open(knob_path(name)) as f:
                out[name] = f.read().strip()
        except OSError:
            pass
    return out


class Plugin:
    _bench_job = None
    _bench_result = None

    # ---------- knobs & markers ----------
    async def get_all(self):
        raw = _read_knobs()
        knobs = {name: raw.get(name) or default
                 for name, default in NUMERIC.items()}
        markers = {m: os.path.exists(marker_path(m)) for m in MARKERS}
        return {"knobs": knobs, "markers": markers}

    async def set_knob(self, name, value):
        if name not in NUMERIC:
            return "unknown knob"
        with open(knob_path(name), "w") as f:
            f.write(str(value))
        return "ok"

    async def reset_knobs(self):
        for name, value in NUMERIC.items():
            with open(knob_path(name), "w") as f:
                f.write(value)
        _log("knobs reset to defaults: " + ", ".join(NUMERIC))
        return "ok"

    async def set_marker(self, name, on):
        # name is not validated here (set_knob checks its whitelist); the
        # panel only ever sends the known marker names
        p = marker_path(name)
        if on:
            with open(p, "a"):
                pass
        elif os.path.exists(p):
            os.remove(p)
        return "ok"

    async def dump_banks(self):
        with open(marker_path("DUMP"), "a"):
            pass
        return "ok"

    # ---------- bench ----------
    async def bench(self):
        return self._read_stats() or {"live": False}

    async def run_bench(self, seconds=10):
        """Starts a benchmark in the background and returns immediately; poll
        bench_poll() for the outcome."""
        if self._bench_job and not self._bench_job.done():
            return {"started": True, "already_running": True}
        seconds = max(3, min(30, int(seconds)))
        self._bench_result = {"state": "running"}
        self._bench_job = asyncio.create_task(self._bench_job_fn(seconds))
        return {"started": True}

    async def bench_poll(self):
        return self._bench_result or {"state": "idle"}

    async def _bench_job_fn(self, seconds):
        """Two modes. Game running: sample the in-game worker's stats (an
        in-game measurement under real load). Game off: run q6bench — the
        production context binary standalone at the current voltage corner."""
        try:
            if not await self._bench_ingame(seconds):
                await self._bench_standalone(seconds)
        except Exception as e:  # noqa: BLE001 — a bench failure must never block the plugin loop
            self._bench_result = {"state": "done", "live": False, "error": str(e)}

    async def _bench_ingame(self, seconds):
        """Sample the running game's worker stats; returns False when the
        worker isn't live (caller falls back to the standalone harness)."""
        s0 = self._read_stats()
        if not (s0 and s0["live"]):
            return False
        loops0, t0 = s0["loops"], time.time()
        samples = []
        for _ in range(seconds):
            await asyncio.sleep(1)
            s = self._read_stats() or {}
            if s.get("exec_us") is not None:
                samples.append(s["exec_us"])
        s1 = self._read_stats() or {}
        dt = time.time() - t0
        if samples and s1.get("live"):
            mean = sum(samples) // len(samples)
            self._bench_result = {
                "state": "done", "live": True, "mode": "ingame",
                "seconds": round(dt, 1), "exec_us": mean,
                "exec_min_us": min(samples), "exec_max_us": max(samples),
                "execs_per_sec": round((s1["loops"] - loops0) / dt, 1),
                "fps_cap": round(1e6 / mean, 1) if mean else None,
            }
        else:
            # in poll results "live" marks the measurement mode, not worker
            # liveness (the frontend keys a done-result off exec_us/error)
            self._bench_result = {"state": "done", "live": True,
                                  "mode": "ingame", "error": "NPU went idle mid-run"}
        return True

    async def _bench_standalone(self, seconds):
        """Run the q6bench harness on the production context binary at the
        current voltage corner."""
        harness = os.path.join(QNN_DIR, "q6bench")
        binf = os.path.join(QNN_DIR, "bin_cfnhwc.bin")
        if not (os.path.exists(harness) and os.path.exists(binf)):
            self._bench_result = {"state": "done", "live": False,
                                  "error": "benchmark tool missing"}
            return
        corner = _read_knobs().get("VCORNER") or "0xA0"
        try:
            p = await asyncio.create_subprocess_exec(
                harness, QNN_DIR, "bin_cfnhwc.bin", str(seconds), corner,
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
            out, _ = await asyncio.wait_for(p.communicate(), timeout=seconds + 45)
        except asyncio.TimeoutError:
            p.kill()
            self._bench_result = {"state": "done", "live": False, "error": "benchmark timeout"}
            return
        except OSError as e:
            self._bench_result = {"state": "done", "live": False, "error": str(e)}
            return
        for ln in reversed((out or b"").decode("utf-8", "replace").splitlines()):
            if ln.startswith("{"):
                try:
                    r = json.loads(ln)
                except ValueError:
                    continue
                if r.get("ok"):
                    r["mode"] = "standalone"
                    r["live"] = True
                else:
                    r = {"live": False, "error": "benchmark failed at: " + str(r.get("step"))}
                r["state"] = "done"
                self._bench_result = r
                return
        self._bench_result = {"state": "done", "live": False, "error": "no benchmark output"}

    def _read_stats(self):
        """Last q6-t7-* worker stats + log freshness (the layer appends these
        ~every second while the game runs)."""
        log = os.path.join(KNOB_DIR, "fsr4npu-SEAM.log")
        try:
            age = time.time() - os.path.getmtime(log)
            tail = _tail(log)
        except OSError:
            return None

        def last(key):
            hits = re.findall(r"^" + key + r" = (\d+)", tail, re.M)
            return int(hits[-1]) if hits else None
        loops = last("q6-t7-loops")
        return {
            "live": age < 10 and loops is not None,
            "age": int(age),
            "exec_us": last("q6-t7-exec-mean-us"),
            "max_us": last("q6-t7-exec-max-us"),
            "copies_us": last("q6-t7-copies-mean-us"),
            "loops": loops,
        }

    # ---------- status & report ----------
    async def soc_info(self):
        return _soc_info()

    async def games(self):
        """Supported-game list. GAMES is a placeholder with a single entry;
        everything downstream is hardwired to No Man's Sky (appid 275850)."""
        out = []
        for g in GAMES:
            st = _checks()
            out.append({"id": g["id"], "name": g["name"],
                        # ready mirrors installed (kept for frontend compatibility)
                        "installed": st["installed"], "ready": st["installed"]})
        return out

    async def status(self):
        return _checks()

    async def make_report(self, fe_errors=""):
        """Compact problem report (hard-capped ~2.5 KB): versions + md5 state,
        chip, live worker stats, and only the ERROR-CLASS lines from the two
        pipeline logs. Nothing leaves the device by itself — the panel puts
        this on the clipboard and the user pastes it into a support request."""
        try:
            with open(os.path.join(PLUGIN_DIR, "package.json")) as f:
                ver = json.load(f).get("version", "?")
        except (OSError, ValueError):
            ver = "?"
        soc = _soc_info()
        st = _checks()
        stats = self._read_stats() or {}
        lines = [f"npupscale {ver} | {soc['machine']} hexagon {soc['hexagon'] or '?'} "
                 f"id {soc['soc_id']} r{soc['revision'] or '?'}",
                 f"state: seat={st['seat']} layer={st['layer']} bins={st['bins']}/{URL_BINS} "
                 f"pass0={st['pass0']}/{URL_PASS0} rt={st['runtime']}/{len(RUNTIME_FILES)} env={st['env'] or st['prefix']}",
                 f"worker: live={stats.get('live')} exec={stats.get('exec_us')}us "
                 f"max={stats.get('max_us')}us loops={stats.get('loops')}"]
        lines.append("knobs: " + " ".join(
            f"{name}={value}" for name, value in _read_knobs().items()))
        bad = re.compile(r"fail|error|died|revert|blacklist|crash|stall|wedge|hang",
                         re.I)
        for log, tag in ((os.path.join(KNOB_DIR, "fsr4npu-SEAM.log"), "seam"),
                         (os.path.join(GAME_BIN, "xess_proxy.log"), "seat")):
            try:
                hits = [ln.strip()[:120] for ln in _tail(log, 1 << 20).splitlines()
                        if bad.search(ln)][-8:]
                if hits:
                    lines.append(f"{tag}-errs:")
                    lines.extend("  " + h for h in hits)
            except OSError:
                pass
        try:
            dmps = sorted(glob.glob(os.path.join(GAME_BIN, "*.dmp")),
                          key=os.path.getmtime)[-3:]
            if dmps:
                lines.append("dumps: " + ", ".join(
                    os.path.basename(d) for d in dmps))
        except OSError:
            pass
        if fe_errors:
            lines.append("panel: " + fe_errors[:400])
        return "\n".join(lines)[:2500]

    # ---------- installer ----------
    def _urls_path(self):
        return os.path.join(CFG_DIR, "urls.json")

    def _read_urls(self):
        urls = {"weights": "", "runtime": ""}
        try:
            with open(self._urls_path()) as f:
                urls.update(json.load(f))
        except (OSError, ValueError):
            # migrate the legacy weights_url.txt, if present
            try:
                with open(os.path.join(CFG_DIR, "weights_url.txt")) as f:
                    urls["weights"] = f.read().strip()
            except OSError:
                pass
        return urls

    async def get_url(self, which="weights"):
        return self._read_urls().get(which, "")

    async def get_urls(self):
        return self._read_urls()

    async def set_url(self, which, url):
        if which not in URL_SETS:
            return "unknown url set"
        os.makedirs(CFG_DIR, exist_ok=True)
        urls = self._read_urls()
        urls[which] = url.strip()
        with open(self._urls_path(), "w") as f:
            json.dump(urls, f)
        return "ok"

    async def file_counts(self):
        return {"weights": len(URL_FILES), "runtime": len(RUNTIME_FILES),
                # 3 always-shipped payload files; q6bench is the conditional 4th
                "payload": 3}

    def _install_archive(self, url, which, files, sums, step, label):
        """Single-archive URL (AMD-style one file). The archive md5 (by its
        basename) is checked when known; every extracted member is md5-gated
        individually before it replaces anything."""
        base = os.path.basename(url)
        os.makedirs(CFG_DIR, exist_ok=True)
        tmp = os.path.join(CFG_DIR, base + ".part")
        try:
            urllib.request.urlretrieve(url, tmp)
        except Exception as e:  # noqa: BLE001
            step(False, f"{label}: download archive: {e}")
            return False
        exp = sums.get(base)
        if exp and _md5(tmp) != exp:
            os.remove(tmp)
            step(False, f"{label}: archive checksum mismatch ({base})")
            return False

        arch = _soc_info()["hexagon"]

        def member_ok(name, dst):
            ext = dst + ".extract"
            expected = _sum(sums, arch, name)
            if expected and _md5(ext) != expected:
                os.remove(ext)
                step(False, f"{label}: checksum mismatch: {name}")
                return False
            os.replace(ext, dst)
            return True

        got = 0
        failed = False
        is_zip = base.endswith(".zip")
        try:
            # normalize to an (archive, member-opener, name-map) triple so the
            # extraction loop below is format-agnostic
            if is_zip:
                archive = zipfile.ZipFile(tmp)
                open_member = archive.open
                names = {os.path.basename(n): n
                         for n in archive.namelist() if not n.endswith("/")}
            else:
                archive = tarfile.open(tmp, "r:*")
                open_member = archive.extractfile
                names = {os.path.basename(m.name): m
                         for m in archive.getmembers() if m.isfile()}
            with archive:
                for name in files:
                    dst = _dst_for(which, name)
                    if os.path.exists(dst) and _md5(dst) == _sum(sums, arch, name):
                        got += 1
                        continue
                    if name not in names:
                        continue
                    src = open_member(names[name])
                    if not src:
                        continue
                    with src, open(dst + ".extract", "wb") as out:
                        shutil.copyfileobj(src, out)
                    if member_ok(name, dst):
                        got += 1
        except Exception as e:  # noqa: BLE001
            step(False, f"{label}: archive extract: {e}")
            failed = True
        finally:
            if os.path.exists(tmp):
                os.remove(tmp)
        complete = got == len(files)
        step(complete, f"{label} archive ({base}): {got}/{len(files)} files")
        return complete and not failed

    async def install(self):
        steps = []

        def step(ok, msg):
            steps.append(("✓" if ok else "✗") + " " + msg)
            _log(msg if ok else "FAIL: " + msg)
            return ok

        with open(os.path.join(PAYLOAD, "checksums.json")) as f:
            sums = json.load(f)

        os.makedirs(KNOB_DIR, exist_ok=True)
        os.makedirs(QNN_DIR, exist_ok=True)
        os.makedirs(LAYER_DIR, exist_ok=True)

        # 1. bundled payload: seat + layer + manifest
        ok = True
        if not os.path.isdir(GAME_BIN):
            # early-failure shape: no "state" key — the frontend checks
            # r.state before using it
            return {"ok": False, "steps": ["✗ No Man's Sky not found — install the game first"]}
        # preserve the game's real XeSS exactly once (the seat forwards to it)
        real = os.path.join(GAME_BIN, "libxess_real.dll")
        seat = os.path.join(GAME_BIN, "libxess.dll")
        if not os.path.exists(real) and os.path.exists(seat):
            shutil.copy2(seat, real)
            step(True, "backed up original XeSS -> libxess_real.dll")
        src = os.path.join(PAYLOAD, "libxess.dll")
        ok &= step(_md5(src) == sums.get("libxess.dll"), "plugin files verified")
        shutil.copy2(src, seat)
        shutil.copy2(os.path.join(PAYLOAD, "fsr4npu_seam.aarch64.so"),
                     os.path.join(LAYER_DIR, "fsr4npu_seam.aarch64.so"))
        shutil.copy2(os.path.join(PAYLOAD, "fsr4npu_seam.json"),
                     os.path.join(LAYER_DIR, "fsr4npu_seam.json"))
        # benchmark harness (plugin code; installed only when present and
        # checksum-valid — the conditional 4th payload file)
        qb = os.path.join(PAYLOAD, "q6bench")
        if os.path.exists(qb) and _md5(qb) == sums.get("q6bench"):
            shutil.copy2(qb, os.path.join(QNN_DIR, "q6bench"))
            os.chmod(os.path.join(QNN_DIR, "q6bench"), 0o755)
        step(True, "installed upscaler into the game")

        # 2. knob defaults (only files that don't exist — repair must not
        #    reset a user's tuned config)
        for name, value in NUMERIC.items():
            p = knob_path(name)
            if not os.path.exists(p):
                with open(p, "w") as f:
                    f.write(value)
        for m in INSTALL_MARKERS:
            p = marker_path(m)
            if not os.path.exists(p):
                with open(p, "a"):
                    pass
        step(True, "settings ready")

        # 3. non-distributables from the custom URLs: models + QNN runtime.
        #    Each URL is EITHER a single archive (AMD-style one file —
        #    .tar.gz/.tgz/.tar/.zip) or a flat directory of loose files.
        urls = self._read_urls()
        arch = _soc_info()["hexagon"]
        for which, files in URL_SETS.items():
            label = "models" if which == "weights" else "runtime"
            url = (urls.get(which) or "").rstrip("/")
            if not url:
                step(False, f"no {label} URL set — {len(files)} files skipped")
                ok = False
                continue
            if url.lower().endswith(ARCHIVE_EXTS):
                have = sum(1 for n in files
                           if os.path.exists(_dst_for(which, n))
                           and _md5(_dst_for(which, n)) == _sum(sums, arch, n))
                if have == len(files):
                    step(True, f"{label} already complete ({have}/{len(files)})")
                else:
                    ok &= self._install_archive(url, which, files, sums, step, label)
            else:
                got = 0
                for name in files:
                    dst = _dst_for(which, name)
                    if os.path.exists(dst) and _md5(dst) == _sum(sums, arch, name):
                        got += 1
                        continue
                    try:
                        tmp = dst + ".part"
                        urllib.request.urlretrieve(url + "/" + name, tmp)
                        expected = _sum(sums, arch, name)
                        if expected and _md5(tmp) != expected:
                            os.remove(tmp)
                            step(False, "checksum mismatch: " + name)
                            ok = False
                            continue
                        os.replace(tmp, dst)
                        got += 1
                    except Exception as e:  # noqa: BLE001
                        step(False, f"download {name}: {e}")
                        ok = False
                step(got == len(files), f"{label} downloaded: {got}/{len(files)}")

        # 4. launch environment: wine prefix registry overrides (self-sufficient)
        #    + the armada tweaks env (applies when the game uses the
        #    armada-game-launch wrapper; harmless when it doesn't)
        reg_state, reg_detail = _write_wine_overrides()
        if reg_state == "missing":
            step(True, "game prefix registry: will apply after first launch")
        else:
            ok &= step(reg_state == "ok",
                       "game prefix registry set" if reg_state == "ok"
                       else f"prefix registry: {reg_detail}")
        try:
            with open(TWEAKS) as f:
                tweaks = json.load(f)
            if not os.path.exists(TWEAKS + ".bak-fsr4npu"):
                shutil.copy2(TWEAKS, TWEAKS + ".bak-fsr4npu")
            env = tweaks.setdefault("games", {}).setdefault("275850", {}).setdefault("env", {})
            for k in SCRUB_ENV:
                env.pop(k, None)
            env.update(TWEAK_ENV)
            with open(TWEAKS, "w") as f:
                json.dump(tweaks, f, indent=1)
            step(True, "game launch environment set")
        except OSError as e:
            step(False, f"launch environment: {e}")
            ok = False

        # 5. runtime sanity (downloaded above if the runtime URL was set)
        state_rt = sum(1 for n in RUNTIME_FILES
                       if os.path.exists(os.path.join(QNN_DIR, n)))
        if state_rt < len(RUNTIME_FILES):
            step(False, f"NPU runtime incomplete: {state_rt}/{len(RUNTIME_FILES)}")
            ok = False

        state = _checks()
        return {"ok": ok and state["installed"], "steps": steps, "state": state}

    async def uninstall(self):
        steps = []
        real = os.path.join(GAME_BIN, "libxess_real.dll")
        seat = os.path.join(GAME_BIN, "libxess.dll")
        if os.path.exists(real):
            shutil.copy2(real, seat)
            steps.append("restored original XeSS")
        m = os.path.join(LAYER_DIR, "fsr4npu_seam.json")
        if os.path.exists(m):
            os.rename(m, m + ".disabled")
            steps.append("layer manifest disabled")
        try:
            if os.path.exists(TWEAKS + ".bak-fsr4npu"):
                shutil.copy2(TWEAKS + ".bak-fsr4npu", TWEAKS)
                steps.append("game tweaks restored from backup")
            else:
                with open(TWEAKS) as f:
                    tweaks = json.load(f)
                env = tweaks.get("games", {}).get("275850", {}).get("env", {})
                for k in SCRUB_ENV:
                    env.pop(k, None)
                with open(TWEAKS, "w") as f:
                    json.dump(tweaks, f, indent=1)
                steps.append("FSR4NPU env removed from tweaks")
        except OSError as e:
            steps.append("tweaks restore failed: " + str(e))
        steps.append(f"Some downloaded files were kept in {QNN_DIR}; "
                     "delete that folder to reclaim space")
        _log("uninstall: " + "; ".join(steps))
        return {"ok": True, "steps": steps}
