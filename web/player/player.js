import { queryEnvironment, executablePath } from "./env.mjs?v=__RECOMP_BUILD__";
import { initNetRelay } from "./net-relay.mjs?v=__RECOMP_BUILD__";
"use strict";
const statusEl = document.getElementById("status");
const canvas = document.getElementById("canvas");
function status(text, back) {
  statusEl.textContent = text;
  if (back) {
    const a = document.createElement("a");
    a.href = "../";
    a.textContent = "\nBack to the launcher";
    statusEl.append(document.createElement("br"), a);
  }
  statusEl.hidden = !text;
}
const log = [];
window.recompLog = log; // for tests
function record(text, warning) {
  log.push(text);
  if (log.length > 2000) log.shift();
  if (text.startsWith("[stream] failed:"))
    status("The hosted game files could not be loaded. Return to the launcher and retry or import your copy.", true);
  (warning ? console.warn : console.log)(text);
}

async function gameId() {
  const parts = location.pathname.split("/").filter(Boolean);
  if (parts.length && parts[parts.length - 1].includes(".")) parts.pop();
  return decodeURIComponent(parts[parts.length - 1] || "");
}

async function start() {
  if (!crossOriginIsolated) throw new Error("This page must be served cross-origin isolated (COOP/COEP headers).");
  if (!navigator.gpu) throw new Error("This browser has no WebGPU.");
  const id = await gameId();
  const gamesUrl = new URL("../games.json", location.href).href;
  const games = await fetch(gamesUrl).then((r) => r.json());
  const game = games.find((g) => g.id === id);
  if (!game) throw new Error(`No game "${id}" in the launcher's games.json.`);
  document.title = game.title;

  // Imported data remains preferred by default. Explicit streaming never
  // modifies that installation, and both modes share the same OPFS profile.
  let imported = false;
  let dir = await navigator.storage.getDirectory();
  try {
    for (const p of [id, "game"]) dir = await dir.getDirectoryHandle(p);
    const components = executablePath(game).split("/");
    const file = components.pop();
    for (const part of components) dir = await dir.getDirectoryHandle(part);
    await dir.getFileHandle(file);
    imported = (await (await dirForStamp(id)).text()).trim() === game.sha256;
  } catch {}
  if (!imported) throw new Error(`${game.title} has not been imported yet.`);
  const source = { kind: "import", args: [`--exe=/opfs/${id}/game/${executablePath(game)}`] };
  window.recompSource = source.kind;

  const adapter = await navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
  if (!adapter) throw new Error("WebGPU gave no adapter.");
  const wanted = ["texture-compression-bc", "depth32float-stencil8", "float32-filterable",
                  "unorm16-texture-formats", "rg11b10ufloat-renderable"];
  const requiredFeatures = wanted.filter((f) => adapter.features.has(f));
  const requiredLimits = {};
  for (const k of ["maxTextureDimension2D", "maxBufferSize", "maxStorageBufferBindingSize",
                   "maxUniformBufferBindingSize", "maxBindGroups", "maxSampledTexturesPerShaderStage",
                   "maxSamplersPerShaderStage", "maxStorageBuffersPerShaderStage",
                   "maxUniformBuffersPerShaderStage", "maxDynamicUniformBuffersPerPipelineLayout",
                   "maxVertexBuffers", "maxVertexAttributes", "maxColorAttachments",
                   "maxInterStageShaderVariables"])
    if (k in adapter.limits) requiredLimits[k] = adapter.limits[k];
  const device = await adapter.requestDevice({ requiredFeatures, requiredLimits });
  device.lost.then((info) => status(`The graphics device was lost: ${info.message}`, true));
  device.addEventListener("uncapturederror", (e) => console.error("WebGPU:", e.error.message));

  const Module = {
    canvas,
    preinitializedWebGPUDevice: device,
    noInitialRun: true,
    // The build stamp keeps a cached module, Wasm or data file from an older
    // build out of this one (tools/web_launcher.py fills it in when staging).
    locateFile: (path, prefix) => `${prefix}${path}?v=__RECOMP_BUILD__`,
    print: (t) => record(t, false),
    printErr: (t) => record(t, true),
    setStatus: (t) => status(t ? t : ""),
    onAbort: (what) => status(`${document.title} has crashed: ${what}`, true),
    // The game's thread ended. Quitting from the game's own menu returns to
    // the launcher; a game that stopped any other way says so and stays on
    // this page with the last error it logged: "could not start" before its
    // window was shown, "has crashed" after.
    onGameExit(code) {
      if (code === 0) {
        status("The game has quit. Returning to the launcher…");
        location.assign("../");
        return;
      }
      const last = [...log].reverse().find((t) => /failed|could not|unavailable|missing|error|fatal/i.test(t) && !/run record/.test(t));
      const started = log.some((t) => /window [0-9a-f]+ shown/.test(t));
      if (!statusEl.querySelector("a"))
        status(started
          ? `${document.title} has crashed (exit code ${code})${last ? `: ${last}` : "."}`
          : `The game could not start${last ? `: ${last}` : "."}`, true);
    },
    preRun: [() => {
      const m = Module;
      m.ENV.RECOMP_PROFILE_DIR = `/opfs/${id}/profile`;
      // TouchMapper places the guest cursor before emitting its click. SDL's
      // additional synthetic mouse click would fire first at the old cursor.
      m.ENV.SDL_TOUCH_MOUSE_EVENTS = "0";
      Object.assign(m.ENV, queryEnvironment(location.search));
    }],
    onRuntimeInitialized() {
      status("");
      setupFullscreenBanner();
      canvas.addEventListener("pointerdown", () => {
        if (window._recompWantPointerLock && document.pointerLockElement !== canvas &&
            canvas.requestPointerLock) {
          Promise.resolve(canvas.requestPointerLock()).catch(() => {});
        }
      });
      canvas.focus();
      initNetRelay(Module);
      Module.callMain(source.args);
    },
  };
  window.Module = Module;
  const script = document.createElement("script");
  script.src = "@APP@.js?v=__RECOMP_BUILD__";
  script.onerror = () => status("The game's web build is missing.", true);
  document.body.append(script);
}

async function dirForStamp(id) {
  const root = await navigator.storage.getDirectory();
  const game = await (await root.getDirectoryHandle(id)).getDirectoryHandle("game");
  return (await game.getFileHandle(".stamp")).getFile();
}

function setupFullscreenBanner() {
  const banner = document.getElementById("fs-banner");
  const textEl = document.getElementById("fs-text");
  const toggleBtn = document.getElementById("fs-toggle");
  const dismissBtn = document.getElementById("fs-dismiss");
  if (!banner || !toggleBtn || !document.fullscreenEnabled) return;
  let compact = false;
  const nativeW = Math.round(window.screen.width * (window.devicePixelRatio || 1));
  const nativeH = Math.round(window.screen.height * (window.devicePixelRatio || 1));
  if (textEl && nativeW >= 640 && nativeH >= 480)
    textEl.textContent = `Tip: Play in Fullscreen for native ${nativeW}×${nativeH} match resolution (Options → Graphics).`;
  const sync = () => {
    const isFs = !!document.fullscreenElement;
    banner.hidden = isFs;
    banner.classList.toggle("compact", compact && !isFs);
    toggleBtn.textContent = isFs ? "Exit Fullscreen" : (compact ? "⛶ Fullscreen" : "Enter Fullscreen");
  };
  const toggleFullscreen = async () => {
    try {
      if (document.fullscreenElement) await document.exitFullscreen();
      else await document.documentElement.requestFullscreen();
    } catch {}
    canvas.focus();
  };
  toggleBtn.addEventListener("click", toggleFullscreen);
  if (dismissBtn) {
    dismissBtn.addEventListener("click", () => {
      compact = true;
      sync();
      canvas.focus();
    });
  }
  document.addEventListener("fullscreenchange", () => {
    sync();
    canvas.focus();
  });
  window.addEventListener("keydown", (e) => {
    if ((e.altKey && e.key === "Enter") || e.key === "F11") {
      e.preventDefault();
      toggleFullscreen();
    }
  });
  sync();
}

canvas.addEventListener("contextmenu", (e) => e.preventDefault());
canvas.addEventListener("click", () => canvas.focus());
start().catch((e) => status(e.message, true));
