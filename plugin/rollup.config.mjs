import resolve from "@rollup/plugin-node-resolve";
import commonjs from "@rollup/plugin-commonjs";
import replace from "@rollup/plugin-replace";
import typescript from "@rollup/plugin-typescript";

// Loader contract: react and react/jsx-runtime arrive as globals (SP_REACT,
// SP_JSX); the virtual modules below hand the bundler those globals.
// @decky/api is bundled from node_modules instead — it handshakes with the
// loader over window at import time, so it needs no virtual entry.
const VIRTUAL_PREFIX = "\0virtual:";
const VIRTUALS = {
  // Named hook imports only — the loader contract for src/index.tsx allows no
  // default react import, so the virtual exposes none.
  react:
    "const R = globalThis.SP_REACT;" +
    "export const useState=R.useState, useEffect=R.useEffect, useRef=R.useRef;",
  // Fragment is unused today (dist keeps only a tree-shaken `J.Fragment;`
  // read) but completes the jsx-runtime surface: a future <></> compiles to it.
  "react/jsx-runtime":
    "const J = globalThis.SP_JSX; export const jsx=J.jsx, jsxs=J.jsxs, Fragment=J.Fragment;",
};
const spGlobals = {
  name: "sp-globals",
  resolveId(id) {
    return Object.prototype.hasOwnProperty.call(VIRTUALS, id) ? VIRTUAL_PREFIX + id : null;
  },
  load(id) {
    if (!id.startsWith(VIRTUAL_PREFIX)) return null;
    return VIRTUALS[id.slice(VIRTUAL_PREFIX.length)];
  },
};

export default {
  input: "src/index.tsx",
  plugins: [
    // Nothing in src or @decky/api reads process.env.NODE_ENV; this pins it
    // to "production" for any dependency that does.
    // preventAssignment stays false: it keeps the legacy plain replacement
    // (no assignment-guarding lookaround), and the plugin warns at buildStart
    // unless the option is exactly true or false.
    replace({ preventAssignment: false, "process.env.NODE_ENV": JSON.stringify("production") }),
    // Order is load-bearing: spGlobals must precede resolve, or node-resolve
    // finds the real react in node_modules and bundles it over the virtual.
    spGlobals,
    resolve({ browser: true }),
    commonjs(),
    typescript(),
  ],
  output: { dir: "dist", format: "esm" },
};
