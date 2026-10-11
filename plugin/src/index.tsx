// NPUpscale — panel front end.
// Layout targets (5.5" display): interactive elements >= 48px tall, cards
// separated by 12px, fixed tab bar at top (not scroll content).
import { useEffect, useRef, useState } from "react";
import { call } from "@decky/api";
import { DONATE_QR } from "./donate-qr";

// Wire mirror — only fields the UI reads are declared; the backend sends more.
// Trimming extras here is compile-time only and leaves the wire format alone.
type KnobState = { knobs: Record<string, string>; markers: Record<string, boolean> };
type InstallState = {
  seat: boolean; layer: boolean; manifest: boolean; prefix: boolean;
  runtime: number; bins: number; pass0: number;
  env: boolean; game: boolean; installed: boolean;
};
type GameEntry = { name: string; installed: boolean };
type Bench = { live: boolean; exec_us?: number | null };
type BenchPoll = {
  state: "idle" | "running" | "done"; mode?: "standalone" | "ingame";
  error?: string; exec_us?: number; exec_min_us?: number;
  exec_max_us?: number; fps_cap?: number | null;
  corner?: string; reps?: number;
};
type Soc = { machine: string; soc_id: string; family: string; revision: string; hexagon: string; support?: "supported" | "needs-pack" | "unknown" };
type Urls = { weights: string; runtime: string };

const DONATE_URL = "https://github.com/sponsors/puzzled-pancake";

const VCORNER_MAX = "0xA0"; // stock voltage corner — grid entry and panel default
const BENCH_SECONDS = 10;   // benchmark duration (s) — also the progress-bar denominator
const BENCH_TICK_MS = 700;  // poll/elapsed tick while a benchmark runs
const POLL_MS = 2500;       // status/bench polling cadence (StatusHero, BenchCard)
const GET_ALL_POLL_MS = 2000; // get_all snapshot cadence (Panel)

// last few panel-side JS errors — included in problem reports (small, capped)
const FE_ERRORS: string[] = [];
const feInstall = () => {
  window.addEventListener("error", (e: any) => FE_ERRORS.push(String(e.message || e).slice(0, 120)));
  window.addEventListener("unhandledrejection", (e: any) => FE_ERRORS.push(String(e.reason?.message || e.reason).slice(0, 120)));
};

const C = {
  green: "#8bc34a", red: "#e2574c", amber: "#e0a832",
  dim: "rgba(255,255,255,0.55)", dim2: "rgba(255,255,255,0.4)",
  card: "rgba(255,255,255,0.06)", cardHi: "rgba(139,195,74,0.10)",
};

const btnBase = {
  display: "block", width: "100%", minHeight: "48px", borderRadius: "10px",
  padding: "12px 16px", cursor: "pointer", fontSize: "15px", margin: "10px 0 0 0",
} as any;
const cornerBtnBase = {
  minHeight: "48px", borderRadius: "10px", color: "#fff", padding: "12px 6px",
  cursor: "pointer", fontSize: "14px",
} as any;
const toggleBase = {
  minHeight: "44px", minWidth: "84px", borderRadius: "10px", color: "#fff",
  padding: "10px 18px", cursor: "pointer", fontSize: "14px", fontWeight: 700,
} as any;
const tabBase = {
  flex: 1, minHeight: "44px", padding: "10px 0", cursor: "pointer", fontSize: "14px",
} as any;

const S = {
  card: { background: C.card, borderRadius: "12px", padding: "14px", margin: "0 0 12px 0" } as any,
  cardGreen: { background: C.cardHi, border: "1px solid rgba(139,195,74,0.35)", borderRadius: "12px", padding: "14px", margin: "0 0 12px 0" } as any,
  // entries spreading a base are already any (the bases are `as any`), so no
  // per-entry cast is needed; btnBack is the shared small-ghost back variant
  btn: { ...btnBase, background: "rgba(255,255,255,0.14)", border: "none", color: "#fff", fontWeight: 600 },
  btnGhost: { ...btnBase, background: "transparent", border: "1px solid rgba(255,255,255,0.25)", color: "#fff" },
  btnBack: { ...btnBase, background: "transparent", border: "1px solid rgba(255,255,255,0.25)", color: "#fff", width: "auto", minHeight: "44px", margin: "0 0 10px 0", padding: "8px 18px" },
  btnDanger: { ...btnBase, background: "transparent", border: "1px solid rgba(226,87,76,0.6)", color: C.red },
  cornerBtn: { ...cornerBtnBase, background: "rgba(255,255,255,0.10)", border: "none", fontWeight: 600 },
  cornerBtnOn: { ...cornerBtnBase, background: "#4c6b22", border: "1px solid rgba(139,195,74,0.6)", fontWeight: 700 },
  toggle: { ...toggleBase, background: "rgba(255,255,255,0.10)", border: "none" },
  toggleOn: { ...toggleBase, background: "#4c6b22", border: "1px solid rgba(139,195,74,0.6)" },
  tab: { ...tabBase, background: "transparent", border: "none", color: C.dim, fontWeight: 600 },
  tabOn: { ...tabBase, background: "rgba(255,255,255,0.10)", border: "none", borderRadius: "10px", color: "#fff", fontWeight: 700 },
  input: {
    width: "100%", minHeight: "44px", background: "rgba(0,0,0,0.4)",
    border: "1px solid rgba(255,255,255,0.2)", borderRadius: "10px", color: "#fff",
    padding: "10px 12px", fontSize: "14px", boxSizing: "border-box", margin: "4px 0",
  } as any,
  between: { display: "flex", alignItems: "center", justifyContent: "space-between", padding: "8px 0" } as any,
  h: { fontSize: "12px", fontWeight: 700, letterSpacing: "1px", textTransform: "uppercase", color: C.dim2, margin: "16px 2px 8px 2px" } as any,
  label: { fontSize: "15px", fontWeight: 600 } as any,
  sub: { fontSize: "13px", color: C.dim, marginTop: "2px" } as any,
  log: { whiteSpace: "pre-wrap", fontSize: "13px", color: C.dim, padding: "12px", background: "rgba(0,0,0,0.35)", borderRadius: "10px", marginTop: "10px" } as any,
};

const fmtMs = (us: number | null | undefined, digits: number) =>
  us === null || us === undefined ? "—" : (us / 1000).toFixed(digits);

// support-badge colors, keyed by the backend's "support" value (always sent)
const BADGE_COLOR: Record<string, string> = { supported: C.green, "needs-pack": C.amber, unknown: C.amber };

/* one polling effect: call fn every ms until unmount. fn lives in a ref so the
 * interval is created once per mount instead of restarting on every render. */
const usePoll = (ms: number, fn: () => Promise<void>) => {
  const fref = useRef(fn);
  fref.current = fn;
  useEffect(() => {
    const t = setInterval(() => { fref.current(); }, ms);
    return () => clearInterval(t);
  }, [ms]);
};

/* shared install/uninstall runner (GamePage, FilesCard): run the job, show its
 * step log, route failures into the problem report. Returns the result, or
 * null when the job failed. */
const runInstallSteps = async (
  job: () => Promise<{ steps: string[]; state?: InstallState }>,
  setSteps: (s: string[]) => void,
): Promise<{ steps: string[]; state?: InstallState } | null> => {
  try {
    const r = await job();
    setSteps(r.steps);
    return r;
  } catch (e: any) {
    FE_ERRORS.push(String(e?.message ?? e).slice(0, 120));
    setSteps(["Couldn't finish — see Settings › Advanced for a problem report."]);
    return null;
  }
};

/* ---------- small pieces ---------- */

/* live readout shared by StatusHero and BenchCard — one source for the
 * "X ms/frame · ceiling ~Y fps" copy and rounding */
const LiveReadout = ({ execUs, prefix }: { execUs?: number | null; prefix?: string }) =>
  execUs ? (
    <>{prefix}{fmtMs(execUs, 1)} ms/frame · ceiling ~{Math.round(1e6 / execUs)} fps</>
  ) : null;

const StatusRow = ({ ok, children }: { ok: boolean; children: any }) => (
  <div style={S.between}>
    <span style={{ fontSize: "14px", opacity: ok ? 1 : 0.6 }}>{children}</span>
    <span style={{ color: ok ? C.green : "#777", fontSize: "15px" }}>{ok ? "✓" : "○"}</span>
  </div>
);

function ToggleRow({ label, sub, on, onChange }: { label: string; sub?: string; on: boolean; onChange: (v: boolean) => void }) {
  return (
    <div style={{ ...S.card, padding: "12px 14px" }}>
      <div style={S.between}>
        <div>
          <div style={S.label}>{label}</div>
          {sub && <div style={S.sub}>{sub}</div>}
        </div>
        <button style={on ? S.toggleOn : S.toggle} onClick={() => onChange(!on)}>{on ? "ON" : "OFF"}</button>
      </div>
    </div>
  );
}

/* Steppers: buttons, not range inputs — a button can't be scroll-dragged the
 * way a range input can. Press-and-hold repeats after 400 ms, then every
 * 100 ms. */
function StepperCard({ label, sub, value, min, max, step, fmt, onChange }: {
  label: string; sub?: string; value: number; min: number; max: number; step: number;
  fmt?: (v: number) => string; onChange: (v: number) => void;
}) {
  const hold = useRef<{ t1?: ReturnType<typeof setTimeout>; t2?: ReturnType<typeof setInterval> }>({});
  const vref = useRef(value);
  vref.current = value;
  const stop = () => { if (hold.current.t1) clearTimeout(hold.current.t1); if (hold.current.t2) clearInterval(hold.current.t2); hold.current = {}; };
  useEffect(() => stop, []);
  const press = (dir: number) => {
    const apply = () => {
      const v = Math.min(max, Math.max(min, Math.round((vref.current + dir * step) / step) * step));
      vref.current = v;   // advance locally so hold-ticks keep moving before the prop round-trips
      onChange(v);
    };
    apply();
    stop();
    hold.current.t1 = setTimeout(() => { apply(); hold.current.t2 = setInterval(apply, 100); }, 400);
  };
  const btn = (dir: number, glyph: string, disabled: boolean) => (
    <button
      style={{ ...S.cornerBtn, flex: 1, fontSize: "22px", fontWeight: 800, opacity: disabled ? 0.35 : 1 }}
      disabled={disabled}
      onPointerDown={(e: any) => { e.preventDefault(); press(dir); }}
      onPointerUp={stop} onPointerLeave={stop} onPointerCancel={stop}
    >{glyph}</button>
  );
  return (
    <div style={{ ...S.card, padding: "12px 14px" }}>
      <div style={S.between}>
        <div>
          <div style={S.label}>{label}</div>
          {sub && <div style={S.sub}>{sub}</div>}
        </div>
        <span style={{ fontSize: "18px", fontWeight: 800, color: C.green }}>{fmt ? fmt(value) : value}</span>
      </div>
      <div style={{ display: "flex", gap: "10px", marginTop: "10px" }}>
        {btn(-1, "−", value <= min)}
        {btn(1, "+", value >= max)}
      </div>
    </div>
  );
}

function QrCode({ size }: { size: number }) {
  const n = DONATE_QR.length;
  const cell = size / n;
  const rects: any[] = [];
  for (let y = 0; y < n; y++) {
    for (let x = 0; x < n; x++) {
      if (DONATE_QR[y][x]) {
        rects.push(<rect key={x + ":" + y} x={x * cell} y={y * cell} width={cell} height={cell} />);
      }
    }
  }
  return (
    <svg width={size} height={size} viewBox={"0 0 " + size + " " + size} style={{ background: "#fff", borderRadius: "6px", display: "block" }}>
      <g fill="#111">{rects}</g>
    </svg>
  );
}

function DonateCard() {
  const [copied, setCopied] = useState(false);
  const [bigQr, setBigQr] = useState(false);
  const copy = async () => {
    setCopied(false);
    try {
      await navigator.clipboard.writeText(DONATE_URL);
      setCopied(true);
    } catch {
      const ta = document.createElement("textarea");
      ta.value = DONATE_URL; document.body.appendChild(ta); ta.select();
      try { document.execCommand("copy"); setCopied(true); } catch { }
      ta.remove();
    }
  };
  const shortUrl = DONATE_URL.replace("https://", "");
  return (
    <div style={{ ...S.card, border: "1px solid rgba(224,168,50,0.35)" }}>
      <div style={{ display: "flex", alignItems: "center", gap: "12px" }}>
        <div style={{ flex: 1 }}>
          <div style={S.label}>NPUpscale is free</div>
          <div style={S.sub}>{copied ? "Link copied, paste it in a browser" : "Support its development"}</div>
          <div style={{ ...S.sub, color: "#cfe3a8", marginTop: "4px" }}>{shortUrl}</div>
        </div>
        <div onClick={() => setBigQr(true)} style={{ cursor: "pointer", padding: "4px" }}>
          <QrCode size={72} />
        </div>
      </div>
      <button style={{ ...S.btn, margin: "10px 0 0 0" }} onClick={copy}>{copied ? "Copied" : "Copy link"}</button>
      {bigQr && (
        <div onClick={() => setBigQr(false)} style={{
          position: "fixed", inset: 0, background: "rgba(0,0,0,0.85)",
          display: "flex", flexDirection: "column", alignItems: "center", justifyContent: "center", zIndex: 50,
        }}>
          <div style={{ background: "#fff", padding: "16px", borderRadius: "12px" }}>
            <QrCode size={240} />
          </div>
          <div style={{ ...S.sub, marginTop: "14px" }}>Scan with your phone, tap anywhere to close</div>
        </div>
      )}
    </div>
  );
}

/* ---------- GAMES tab ---------- */

function StatusHero({ onSetup }: { onSetup: () => void }) {
  const [bench, setBench] = useState<Bench | null>(null);
  // single-game era: only games[0] is rendered; a real multi-game list view is
  // a product decision, not cleanup.
  const [games, setGames] = useState<GameEntry[]>([]);
  usePoll(POLL_MS, async () => {
    try { setGames(await call<[], GameEntry[]>("games")); } catch { }
    try { setBench(await call<[], Bench>("bench")); } catch { }
  });
  const g = games[0];
  if (bench?.live && bench?.exec_us) {
    return (
      <div style={S.cardGreen}>
        <div style={{ fontSize: "17px", fontWeight: 700, color: C.green }}>Upscaling is running</div>
        <div style={{ ...S.sub, marginTop: "6px" }}>
          <LiveReadout execUs={bench.exec_us} />
        </div>
      </div>
    );
  }
  if (g?.installed) {
    return (
      <div style={S.cardGreen}>
        <div style={{ fontSize: "17px", fontWeight: 700, color: C.green }}>Ready to play</div>
        <div style={{ ...S.sub, marginTop: "6px" }}>
          Launch {g.name} — upscaling turns on by itself.
        </div>
      </div>
    );
  }
  if (g) {
    return (
      <div style={{ ...S.card, border: "1px solid rgba(224,168,50,0.4)" }}>
        <div style={{ fontSize: "17px", fontWeight: 700, color: C.amber }}>Finish setup</div>
        <div style={{ ...S.sub, marginTop: "6px" }}>One game needs its files installed.</div>
        <button style={S.btn} onClick={onSetup}>Set up {g.name}</button>
      </div>
    );
  }
  return <div style={S.card}><div style={S.sub}>Checking…</div></div>;
}

function GamePage({ onBack }: { onBack: () => void }) {
  const [state, setState] = useState<InstallState | null>(null);
  const [steps, setSteps] = useState<string[]>([]);
  const [busy, setBusy] = useState(false);
  const [confirmRemove, setConfirmRemove] = useState(false);
  const load = async () => { try { setState(await call<[], InstallState>("status")); } catch { } };
  useEffect(() => { load(); }, []);
  const run = async (what: "install" | "uninstall") => {
    setBusy(true); setSteps([]);
    const r = await runInstallSteps(
      () => call<[string], { steps: string[]; state?: InstallState }>(what),
      setSteps,
    );
    if (r?.state) setState(r.state);
    setBusy(false);
    setConfirmRemove(false);
  };
  return (
    <div>
      <button style={S.btnBack} onClick={onBack}>‹ Games</button>
      <div style={S.card}>
        <div style={{ fontSize: "17px", fontWeight: 700 }}>No Man's Sky</div>
        <div style={{ ...S.sub, marginBottom: "6px" }}>The upscaler installs into this game.</div>
        {state === null ? <div style={S.sub}>Checking…</div> : (
          <div>
            <StatusRow ok={state.game}>Game found</StatusRow>
            <StatusRow ok={state.seat}>Upscaler installed</StatusRow>
            <StatusRow ok={state.layer && state.manifest}>NPU components active</StatusRow>
            <StatusRow ok={state.env || state.prefix}>Game launch configured</StatusRow>
          </div>
        )}
      </div>
      <button style={S.btn} disabled={busy} onClick={() => run("install")}>
        {busy ? "Working…" : state?.installed ? "Repair" : "Install"}
      </button>
      {confirmRemove ? (
        <div>
          <div style={{ ...S.sub, textAlign: "center", margin: "10px 0 0 0" }}>Remove the upscaler from this game?</div>
          <button style={S.btnDanger} disabled={busy} onClick={() => run("uninstall")}>Yes, remove</button>
          <button style={S.btnGhost} onClick={() => setConfirmRemove(false)}>Keep it</button>
        </div>
      ) : (
        <button style={S.btnGhost} disabled={busy} onClick={() => setConfirmRemove(true)}>Remove</button>
      )}
      {steps.length > 0 && <div style={S.log}>{steps.join("\n")}</div>}
      <div style={{ ...S.sub, textAlign: "center", margin: "14px 0" }}>
        Image settings are shared across games (Settings tab).
      </div>
    </div>
  );
}

// Panel owns the game-page switch (gamePage): both the hero card and the list
// card route through onOpenGame so there is one navigation state.
function GamesTab({ onOpenGame }: { onOpenGame: () => void }) {
  return (
    <div>
      <StatusHero onSetup={onOpenGame} />
      <div style={S.h}>Games</div>
      <div style={{ ...S.card, cursor: "pointer" }} onClick={onOpenGame}>
        <div style={S.between}>
          <div>
            <div style={{ fontSize: "16px", fontWeight: 700 }}>No Man's Sky</div>
            <div style={S.sub}>Neural upscaling on the NPU</div>
          </div>
          <span style={{ color: C.green, fontSize: "22px" }}>›</span>
        </div>
      </div>
      <div style={{ ...S.sub, padding: "4px 2px" }}>More games are coming.</div>
      <DonateCard />
    </div>
  );
}

/* ---------- NPU tab ---------- */

function ChipCard() {
  const [soc, setSoc] = useState<Soc | null>(null);
  useEffect(() => { call<[], Soc>("soc_info").then(setSoc).catch(() => { }); }, []);
  if (!soc) return <div style={S.card}><div style={S.sub}>Reading chip…</div></div>;
  const meta = [
    soc.soc_id ? `id ${soc.soc_id}` : "",
    soc.revision ? `rev ${soc.revision}` : "",
    soc.family || "",
  ].filter(Boolean).join(" · ");
  const sup = soc.support; // backend always sends it — no fallback needed
  const badge = sup === "supported" ? "✓ supported"
    : sup === "needs-pack" ? `needs ${soc.hexagon} model pack`
    : "chip not verified";
  const badgeC = (sup && BADGE_COLOR[sup]) || C.amber;
  return (
    <div style={S.card}>
      <div style={S.between}>
        <span style={{ fontSize: "18px", fontWeight: 800 }}>{soc.machine || soc.family || "Chip"}</span>
        <span style={{ color: badgeC, fontSize: "13px", fontWeight: 700 }}>{badge}</span>
      </div>
      <div style={{ ...S.sub, marginTop: "4px" }}>
        Hexagon {soc.hexagon || "?"} NPU{meta ? ` · ${meta}` : ""}
      </div>
      {sup !== "supported" && (
        <div style={{ ...S.sub, marginTop: "6px" }}>
          This build ships {soc.hexagon ? `v73 model files — yours needs a ${soc.hexagon} build.` : "model files for Hexagon v73 chips."}
        </div>
      )}
    </div>
  );
}

function FilesCard() {
  const [state, setState] = useState<InstallState | null>(null);
  const [urls, setUrls] = useState<Urls | null>(null);
  const [editing, setEditing] = useState(false);
  const [steps, setSteps] = useState<string[]>([]);
  const [busy, setBusy] = useState(false);
  const load = async () => {
    try { setState(await call<[], InstallState>("status")); } catch { }
    try { setUrls(await call<[], Urls>("get_urls")); } catch { }
  };
  useEffect(() => { load(); }, []);
  const saveAndFetch = async () => {
    setBusy(true); setSteps([]);
    const r = await runInstallSteps(async () => {
      if (urls) {
        await call("set_url", "weights", urls.weights);
        await call("set_url", "runtime", urls.runtime);
      }
      return await call<[string], { steps: string[]; state?: InstallState }>("install");
    }, setSteps);
    if (r?.state) setState(r.state);
    setBusy(false);
    setEditing(false);
  };
  const done = state && state.bins >= 6 && state.pass0 >= 6 && state.runtime >= 8;
  return (
    <div style={S.card}>
      <div style={S.between}>
        <span style={S.label}>Licensed files</span>
        <span style={{ color: done ? C.green : C.amber, fontSize: "13px", fontWeight: 700 }}>
          {done ? "complete" : "needed"}
        </span>
      </div>
      {state && (
        <div>
          <StatusRow ok={state.bins >= 6}>Model files ({state.bins}/6)</StatusRow>
          <StatusRow ok={state.pass0 >= 6}>AI weights ({state.pass0}/6)</StatusRow>
          <StatusRow ok={state.runtime >= 8}>NPU runtime ({state.runtime}/8)</StatusRow>
        </div>
      )}
      <div style={{ ...S.sub, marginTop: "6px" }}>
        These live on your own server — the license doesn't allow bundling them.
      </div>
      {editing ? (
        <div>
          <div style={{ ...S.sub, marginTop: "10px" }}>Model files URL (.tar.gz or folder)</div>
          <input style={S.input} value={urls?.weights ?? ""} placeholder="https://your-server/models.tar.gz"
            onChange={(e: any) => setUrls((p) => ({ ...(p ?? { weights: "", runtime: "" }), weights: e.target.value }))} />
          <div style={S.sub}>NPU runtime URL (.tar.gz or folder)</div>
          <input style={S.input} value={urls?.runtime ?? ""} placeholder="https://your-server/runtime.tar.gz"
            onChange={(e: any) => setUrls((p) => ({ ...(p ?? { weights: "", runtime: "" }), runtime: e.target.value }))} />
          <button style={S.btn} disabled={busy} onClick={saveAndFetch}>{busy ? "Working…" : "Save & download"}</button>
          <button style={S.btnGhost} onClick={() => setEditing(false)}>Cancel</button>
        </div>
      ) : (
        <button style={S.btn} onClick={() => setEditing(true)}>Change URLs</button>
      )}
      {steps.length > 0 && <div style={S.log}>{steps.join("\n")}</div>}
    </div>
  );
}

function VoltageCard({ onKnob, vc }: { onKnob: (k: string, v: string) => void; vc: string }) {
  const CORNERS = [
    [VCORNER_MAX, "MAX · stock"], ["0x60", "NOM"], ["0x50", "SVS+"],
    ["0x40", "SVS"], ["0x30", "SVS2"], ["0x20", "MIN"],
  ];
  return (
    <div style={S.card}>
      <div style={S.label}>Voltage</div>
      <div style={{ ...S.sub, marginBottom: "10px" }}>MAX is stock · lower corners save battery · applies in ~1 s</div>
      <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr 1fr", gap: "10px" }}>
        {CORNERS.map(([c, l]) => (
          <button key={c} style={vc === c ? S.cornerBtnOn : S.cornerBtn} onClick={() => onKnob("VCORNER", c)}>
            {l}
          </button>
        ))}
      </div>
    </div>
  );
}

function BenchCard() {
  const [bench, setBench] = useState<Bench | null>(null);
  const [job, setJob] = useState<BenchPoll | null>(null);
  const [elapsed, setElapsed] = useState(0);
  usePoll(POLL_MS, async () => { try { setBench(await call<[], Bench>("bench")); } catch { } });
  useEffect(() => { call<[], BenchPoll>("bench_poll").then(setJob).catch(() => { }); }, []);
  useEffect(() => {
    if (job?.state !== "running") return;
    const t = setInterval(async () => {
      try { setJob(await call<[], BenchPoll>("bench_poll")); } catch { }
      setElapsed((e) => e + BENCH_TICK_MS / 1000);
    }, BENCH_TICK_MS);
    return () => clearInterval(t);
  }, [job?.state]);
  const start = async () => {
    setElapsed(0);
    try {
      await call<[number], { started: boolean }>("run_bench", BENCH_SECONDS);
      setJob({ state: "running" });
    } catch (e: any) {
      // error is a truthy flag only — the result view renders a fixed calm message
      setJob({ state: "done", error: String(e) });
    }
  };
  const running = job?.state === "running";
  const res = job?.state === "done" ? job : null;
  return (
    <div style={S.card}>
      <div style={S.between}>
        <span style={S.label}>Benchmark</span>
        <span style={{ color: bench?.live ? C.green : "#777", fontSize: "13px" }}>
          {bench?.live ? "● in game" : "○ idle"}
        </span>
      </div>
      {!running && !res && (
        <div style={{ ...S.sub, marginTop: "4px" }}>
          {bench?.exec_us
            ? <LiveReadout execUs={bench.exec_us} prefix="Now: " />
            : "Runs the upscaler itself — game not required."}
        </div>
      )}
      {running && (
        <div style={{ marginTop: "10px" }}>
          <div style={{ ...S.sub }}>Measuring… {elapsed.toFixed(0)}s</div>
          <div style={{ height: "8px", background: "rgba(255,255,255,0.08)", borderRadius: "4px", marginTop: "8px", overflow: "hidden" }}>
            <div style={{ width: `${Math.min(100, (elapsed / BENCH_SECONDS) * 100)}%`, height: "100%", background: C.green }} />
          </div>
        </div>
      )}
      {res && (res.exec_us ? (
        <div style={{ marginTop: "10px" }}>
          <div style={{ fontSize: "26px", fontWeight: 800 }}>
            {fmtMs(res.exec_us!, 2)}<span style={{ fontSize: "14px", fontWeight: 600, color: C.dim }}> ms/frame</span>
          </div>
          <div style={{ fontSize: "17px", fontWeight: 700, color: C.green, marginTop: "2px" }}>
            ≈ {res.fps_cap ?? Math.round(1e6 / res.exec_us!)} fps ceiling
          </div>
          <div style={S.sub}>
            {res.mode === "standalone" ? "direct" : "in game"}{res.corner ? ` · ${res.corner}` : ""}{res.reps ? ` · ${res.reps} runs` : ""}
            {res.exec_min_us ? ` · best ${fmtMs(res.exec_min_us, 2)}` : ""}
            {res.exec_max_us ? ` · worst ${fmtMs(res.exec_max_us, 2)}` : ""}
          </div>
        </div>
      ) : (
        <div style={{ ...S.log, marginTop: "10px" }}>{res.error ? "The benchmark couldn't finish — try running it again." : "No result."}</div>
      ))}
      <button style={S.btn} disabled={running} onClick={start}>{running ? "Running…" : "Run benchmark"}</button>
    </div>
  );
}

function NpuTab({ onKnob, vc }: { onKnob: (k: string, v: string) => void; vc: string }) {
  return (
    <div>
      <div style={S.h}>Chip</div>
      <ChipCard />
      <div style={S.h}>Files</div>
      <FilesCard />
      <div style={S.h}>Voltage</div>
      <VoltageCard onKnob={onKnob} vc={vc} />
      <div style={S.h}>Speed</div>
      <BenchCard />
    </div>
  );
}

/* ---------- SETTINGS tab ---------- */

function ReportCard() {
  const [text, setText] = useState<string | null>(null);
  const [copied, setCopied] = useState(false);
  const gen = async () => {
    setCopied(false);
    try { setText(await call<[string], string>("make_report", FE_ERRORS.slice(-8).join(" | "))); }
    catch (e: any) { FE_ERRORS.push(String(e?.message ?? e).slice(0, 120)); setText("Couldn't build a report."); }
  };
  const copy = async () => {
    if (!text) return;
    try {
      await navigator.clipboard.writeText(text);
      setCopied(true);
    } catch {
      const ta = document.createElement("textarea");
      ta.value = text; document.body.appendChild(ta);
      ta.select();
      try { document.execCommand("copy"); setCopied(true); } catch { }
      ta.remove();
    }
  };
  return (
    <div style={S.card}>
      <div style={S.label}>Report a problem</div>
      <div style={{ ...S.sub, marginTop: "2px" }}>
        Builds a short summary (versions, chip, state, recent errors) — nothing is sent anywhere until you paste it.
      </div>
      <button style={S.btn} onClick={gen}>{text ? "Rebuild report" : "Build report"}</button>
      {text && (
        <div>
          <div style={S.log}>{text}</div>
          <button style={S.btn} onClick={copy}>{copied ? "Copied ✓" : "Copy report"}</button>
        </div>
      )}
    </div>
  );
}

function SettingsTab({ st, setSt }: { st: KnobState; setSt: (v: KnobState | ((p: KnobState) => KnobState)) => void }) {
  const [advanced, setAdvanced] = useState(false);
  // get_all is polled once by Panel and handed down here; marker/reset below
  // still refetch immediately so a toggle reflects without waiting a tick.
  const f = (k: string, d: number) => parseFloat(st.knobs[k] ?? String(d));
  const setKnob = async (k: string, v: number | string) => {
    const s = typeof v === "number" ? String(Number(v.toFixed(3))) : v;
    setSt((p) => ({ ...p, knobs: { ...p.knobs, [k]: s } }));
    await call("set_knob", k, s);
  };
  const marker = async (m: string, on: boolean) => {
    await call("set_marker", m, on);
    try { setSt(await call<[], KnobState>("get_all")); } catch { }
  };
  const reset = async () => {
    await call("reset_knobs");
    try { setSt(await call<[], KnobState>("get_all")); } catch { }
  };
  return (
    <div>
      <div style={{ ...S.sub, margin: "4px 2px 12px 2px" }}>
        These settings apply to every game.
      </div>
      <StepperCard label="Exposure" sub="Light level fed to the upscaler" value={f("EXPO", 4.0)} min={0.5} max={8} step={0.25}
        fmt={(v: number) => v.toFixed(2)} onChange={(v: number) => setKnob("EXPO", v)} />
      <StepperCard label="Sharpness" value={f("SHARP", 0.5)} min={0} max={1} step={0.05}
        fmt={(v: number) => v.toFixed(2)} onChange={(v: number) => setKnob("SHARP", v)} />
      <StepperCard label="Edge smoothing" value={f("BFLOOR", 0)} min={0} max={1} step={0.05}
        fmt={(v: number) => v.toFixed(2)} onChange={(v: number) => setKnob("BFLOOR", v)} />
      <StepperCard label="Smoothing hold" value={f("BFTAU", 0.35)} min={0} max={1} step={0.05}
        fmt={(v: number) => v.toFixed(2)} onChange={(v: number) => setKnob("BFTAU", v)} />
      <ToggleRow label="Auto exposure" sub="Overrides the Exposure setting" on={!!st.markers["AUTOEXPO"]} onChange={(v) => marker("AUTOEXPO", v)} />
      <ToggleRow label="Motion clarity" sub="Movement-based smoothing" on={f("DILATE", 0) > 0.5}
        onChange={(v) => setKnob("DILATE", v ? 1 : 0)} />
      <button style={S.btnGhost} onClick={reset}>Reset to defaults</button>
      <div style={S.h}>Advanced</div>
      <button style={{ ...S.btnGhost, margin: "0 0 12px 0" }} onClick={() => setAdvanced(!advanced)}>
        {advanced ? "Hide" : "Show"} advanced controls
      </button>
      {advanced && (
        <div>
          <ToggleRow label="Zero-copy memory" sub="Frames stream straight to the NPU"
            on={!!st.markers["ZCC2"] && !!st.markers["ZCC3"]}
            onChange={(v) => marker("ZCC2", v).then(() => marker("ZCC3", v))} />
          <button style={S.btnGhost} onClick={() => call("dump_banks")}>Save NPU debug dump</button>
          <ReportCard />
        </div>
      )}
    </div>
  );
}

/* ---------- shell ---------- */

// The loader calls the default export as a plain function to build the plugin
// descriptor — hooks here would throw React error #321 (Invalid hook call).
// Hooks live in components; the descriptor only hands React an element.
function Panel() {
  const [tab, setTab] = useState<"games" | "npu" | "settings">("games");
  const [gamePage, setGamePage] = useState(false);
  // Panel is the single get_all poller (2 s); the snapshot feeds both the
  // VCORNER mirror here and the Settings tab below.
  const [st, setSt] = useState<KnobState>({ knobs: {}, markers: {} });
  useEffect(() => { feInstall(); }, []);
  usePoll(GET_ALL_POLL_MS, async () => { try { setSt(await call<[], KnobState>("get_all")); } catch { } });
  // state mirror for the panel knob — VoltageCard is the only onKnob caller and
  // always passes VCORNER; set_knob itself passes any knob through.
  const onKnob = async (k: string, v: string) => {
    setSt((p) => ({ ...p, knobs: { ...p.knobs, [k]: v } }));
    await call("set_knob", k, v);
  };
  const vc = st.knobs["VCORNER"] || VCORNER_MAX;
  return (
    <div>
      <div style={{ display: "flex", gap: "6px", marginBottom: "14px" }}>
        <button style={tab === "games" ? S.tabOn : S.tab} onClick={() => { setTab("games"); setGamePage(false); }}>Games</button>
        <button style={tab === "npu" ? S.tabOn : S.tab} onClick={() => setTab("npu")}>NPU</button>
        <button style={tab === "settings" ? S.tabOn : S.tab} onClick={() => setTab("settings")}>Settings</button>
      </div>
      {tab === "games" && !gamePage && <GamesTab onOpenGame={() => setGamePage(true)} />}
      {tab === "games" && gamePage && <GamePage onBack={() => setGamePage(false)} />}
      {tab === "npu" && <NpuTab onKnob={onKnob} vc={vc} />}
      {tab === "settings" && <SettingsTab st={st} setSt={setSt} />}
    </div>
  );
}

export default function () {
  return {
    name: "NPUpscale",
    version: "1.5.0",
    description: "Neural upscaling on your NPU — pick a game, everything configures itself.",
    content: <Panel />,
    onDismount() { },
  };
}
