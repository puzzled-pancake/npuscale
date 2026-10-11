// Smoke gate: the loader calls the default export as a plain function.
// trap stands in for React's hook dispatcher, so any hook call during that
// invocation throws HOOK-CALLED-OUTSIDE-RENDER here — the same invalid hook
// call the loader surfaces as Steam UI's "Minified React error #321".
const trap = () => { throw new Error("HOOK-CALLED-OUTSIDE-RENDER"); };
// Mirrors the virtual react module in rollup.config.mjs: these are the only hook
// names the bundle can import, so the only ones that could ever fire here.
const R = {
  useState: trap,
  useEffect: trap,
  useRef: trap,
};
globalThis.SP_REACT = R;
// Fake-element factory for jsx/jsxs; FRAGMENT is the Fragment marker.
const el = (t, p) => ({ __el: true, type: t, props: p ?? {} });
const FRAGMENT = "F";
globalThis.SP_JSX = {
  Fragment: FRAGMENT,
  jsx: el,
  jsxs: el,
};
// The @decky/api shim reads the loader-init global off `window` during import,
// so `window` must exist before ./dist/index.js loads.
globalThis.window = globalThis;
globalThis.__DECKY_SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED_deckyLoaderAPIInit = {
  connect: () => ({ _version: 2, call: async () => ({}) }),
};
const m = await import("./dist/index.js");
const d = m.default();
if (!d || d.name !== "NPUpscale" || !d.content?.__el || typeof d.content?.type !== "function") {
  console.error("BAD DESCRIPTOR", JSON.stringify(d, (k, v) => typeof v === "function" ? "[fn]" : v));
  process.exit(1);
}
console.log("SMOKE-PASS: descriptor ok, content element wraps the component, zero hook calls at definition time");
