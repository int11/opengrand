// Dev server: serves dist/ and bridges ws://host:8079/midi to a realtime
// firmware simulator process (one per WebSocket client, on the PANEL port).
//   npm run build && npm run dev   -> open http://localhost:8079
// localhost is a secure context: Web MIDI, Web Bluetooth and the PWA service worker work over plain http here.
// Minimal RFC 6455 implementation (binary frames only), no dependencies.
import { createHash } from "node:crypto";
import { createReadStream, existsSync, statSync } from "node:fs";
import { createServer } from "node:http";
import type { Duplex } from "node:stream";
import { extname, join, normalize, resolve } from "node:path";

import { toHex } from "../src/protocol/sysex.ts";
import { SimProcessLink } from "./sim-link.ts";

const PORT = Number(process.env.PORT ?? 8079);
const ROOT = resolve("dist");
const TYPES: Record<string, string> = {
  ".html": "text/html",
  ".js": "text/javascript",
  ".css": "text/css",
  ".map": "application/json",
  ".webmanifest": "application/manifest+json",
  ".png": "image/png",
};

const server = createServer((req, res) => {
  const url = new URL(req.url ?? "/", "http://x");
  const file = normalize(join(ROOT, url.pathname === "/" ? "index.html" : url.pathname));
  if (!file.startsWith(ROOT) || !existsSync(file) || !statSync(file).isFile()) {
    res.writeHead(404).end("not found");
    return;
  }
  // index.html and sw.js must not be served stale (the service worker does its own caching)
  const fresh = /(^|\/)(index\.html|sw\.js)$/.test(file) ? { "cache-control": "no-cache" } : {};
  res.writeHead(200, { "content-type": TYPES[extname(file)] ?? "application/octet-stream", ...fresh });
  createReadStream(file).pipe(res);
});

function wsSend(sock: Duplex, payload: Uint8Array): void {
  const n = payload.length;
  const head = n < 126 ? Buffer.from([0x82, n]) : Buffer.from([0x82, 126, n >> 8, n & 0xff]);
  sock.write(Buffer.concat([head, payload]));
}

server.on("upgrade", (req, sock) => {
  if (req.url !== "/midi") return sock.destroy();
  const key = req.headers["sec-websocket-key"];
  const accept = createHash("sha1").update(`${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`).digest("base64");
  sock.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);

  const sim = new SimProcessLink({ port: "panel" });
  sim.onMessage((m) => wsSend(sock, m));
  sim.onLine((l) => console.log(`[sim] ${l}`));
  console.log("client connected — simulator started");

  let buf = Buffer.alloc(0);
  sock.on("data", (chunk: Buffer) => {
    buf = Buffer.concat([buf, chunk]);
    while (buf.length >= 2) {
      const op = buf[0] & 0x0f;
      let len = buf[1] & 0x7f;
      let off = 2;
      if (len === 126) {
        if (buf.length < 4) return;
        len = buf.readUInt16BE(2);
        off = 4;
      } else if (len === 127) {
        return sock.destroy(); // messages are tiny; 64-bit lengths never used
      }
      const masked = (buf[1] & 0x80) !== 0;
      const total = off + (masked ? 4 : 0) + len;
      if (buf.length < total) return;
      const mask = masked ? buf.subarray(off, off + 4) : null;
      const data = Buffer.from(buf.subarray(total - len, total));
      if (mask) for (let i = 0; i < data.length; i++) data[i] ^= mask[i & 3];
      buf = buf.subarray(total);
      if (op === 0x8) return sock.end();
      if (op === 0x2) {
        console.log(`[app] ${toHex(data)}`);
        sim.send(new Uint8Array(data));
      }
    }
  });
  sock.on("close", () => sim.close());
  sock.on("error", () => sim.close());
});

server.listen(PORT, () => console.log(`OpenGrand dev server: http://localhost:${PORT}  (ws /midi -> firmware simulator)`));
