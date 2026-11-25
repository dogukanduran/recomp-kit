// WebSocket relay client for virtual WSOCK32 / WS2_32 networking in the browser.
// Connects to /net on the same origin (or ?net=ws://... override) and bridges
// binary frames between wsock.cpp (WASM) and the relay server.
"use strict";

export function initNetRelay(Module, statusCallback) {
  const params = new URLSearchParams(location.search);
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  const url = params.get("net") || `${proto}//${location.host}/net`;

  let ws = null;
  let connected = false;
  let assignedIp = null;
  const txQueue = [];

  function deliverToWasm(bytes) {
    if (!Module._recomp_wsock_on_packet || !Module._recomp_wsock_rx_buf || !Module.HEAPU8)
      return;
    const ptr = Module._recomp_wsock_rx_buf(bytes.length);
    if (!ptr) return;
    Module.HEAPU8.set(bytes, ptr);
    Module._recomp_wsock_on_packet(ptr, bytes.length);
  }

  function flushQueue() {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    while (txQueue.length) {
      ws.send(txQueue.shift());
    }
  }

  function connect() {
    try {
      ws = new WebSocket(url);
    } catch (e) {
      console.warn("[net-relay] WebSocket init failed:", e);
      return;
    }
    ws.binaryType = "arraybuffer";
    ws.onopen = () => {
      connected = true;
      console.log(`[net-relay] connected to ${url}`);
      flushQueue();
    };
    ws.onmessage = (ev) => {
      if (!(ev.data instanceof ArrayBuffer)) return;
      const bytes = new Uint8Array(ev.data);
      if (bytes.length >= 5 && bytes[0] === 0x80) {
        assignedIp = `${bytes[1]}.${bytes[2]}.${bytes[3]}.${bytes[4]}`;
        console.log(`[net-relay] virtual LAN IP: ${assignedIp}`);
        if (statusCallback) statusCallback(assignedIp);
      }
      deliverToWasm(bytes);
    };
    ws.onclose = () => {
      connected = false;
      setTimeout(connect, 3000);
    };
    ws.onerror = () => {
      // onclose handles reconnect
    };
  }

  window.recompNet = {
    get virtualIp() {
      return assignedIp;
    },
    get connected() {
      return connected;
    },
    send(u8) {
      const copy = new Uint8Array(u8);
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(copy);
      } else {
        if (txQueue.length < 256) txQueue.push(copy);
      }
    },
  };

  connect();
}
