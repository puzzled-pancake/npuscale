#!/bin/bash
# waits for NMS to exit, then rips the fsr4npu install from the game dir,
# the vulkan layer dir, the qnn dir, game-tweaks.json and the wine prefix.
# Keeps libxess_real.dll and the non-fsr4npu files in fsr4npu-logs (see the
# keep-rules below). Requires a pre-taken backup; flags RIP-DONE when clean.
# Run under sudo: it rewrites /etc/armada/game-tweaks.json.
K=/var/home/armada/fsr4npu-logs
FLAG="$K/RIP-DONE"
rm -f "$FLAG"
while pgrep -f "NMS[.]exe" >/dev/null 2>&1; do sleep 10; done
G="/var/home/armada/.local/share/Steam/steamapps/common/No Man's Sky/Binaries"
L=/var/home/armada/.local/share/vulkan/implicit_layer.d
Q=/var/home/armada/.local/share/fsr4npu/qnn

# game files — keep libxess_real.dll (the game's own XeSS); remove every
# other libxess variant plus pass0 captures, markers, dumps and logs
cd "$G" || exit 1
for f in libxess.dll libxess_*.bak libxess_*.dll \
         pass0_*.raw xess_proxy.log fsr4npu-* fsr4npu_*.raw; do
  [ "$f" = "libxess_real.dll" ] && continue
  rm -f "$f"
done
echo "game files cleaned"
rm -f "$L"/fsr4npu_seam.aarch64.so "$L"/fsr4npu_seam.json
echo "layer removed"
rm -rf "$Q"
echo "qnn removed"
rm -f "$K"/fsr4npu-*
echo "logs cleaned"

# tweaks env: remove the TWEAK_ENV keys the installer adds, plus FSR4NPU_VKTRACE
# (matches SCRUB_ENV in fsr4npu-knobs/main.py — that list is the source of
# truth, keep in sync)
# wine prefix: remove the DLL overrides the installer adds
python3 - <<'PYEOF'
import json, os, re, sys, traceback

APPID = "275850"

def write_atomic(p, text, **kw):
    tmp = p + ".rip-tmp"
    with open(tmp, "w", **kw) as f:
        f.write(text)
    os.replace(tmp, p)

err = False

try:
    p = "/etc/armada/game-tweaks.json"
    with open(p) as f:
        d = json.load(f)
    env = d.get("games", {}).get(APPID, {}).get("env")
    if env is not None:
        for k in ["ENABLE_FSR4NPU_SEAM", "FSR4NPU_NPU", "FSR4NPU_SPOOF",
                  "FSR4NPU_SPOOF_FIELDS", "FSR4NPU_SPOOF_REVERT_MS", "SL_LOG_LEVEL",
                  "SL_LOG_PATH", "WINEDLLOVERRIDES",
                  "DISABLE_VK_LAYER_VALVE_steam_fossilize_1", "FSR4NPU_VKTRACE"]:
            env.pop(k, None)
        write_atomic(p, json.dumps(d, indent=1))
        print("tweaks cleaned")
except Exception:
    traceback.print_exc()
    err = True

try:
    p = ("/var/home/armada/.local/share/Steam/steamapps/compatdata/"
         "%s/pfx/user.reg" % APPID)
    with open(p, encoding="utf-8") as f:
        s = f.read()
    for k in ["nvngx", "vulkan-1", "nvapi64", "combase", "wintrust", "version"]:
        s = re.sub(r'^"%s"="[^"]*"\n' % re.escape(k), "", s, flags=re.M)
    write_atomic(p, s, encoding="utf-8")
    print("registry cleaned")
except Exception:
    traceback.print_exc()
    err = True

sys.exit(1 if err else 0)
PYEOF

echo done > "$FLAG"
