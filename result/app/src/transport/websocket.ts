// WebSocket transport to the firmware simulator (scripts/dev-server.mjs bridges
// ws://host/midi <-> the simulator's stdin/stdout). Each WS binary frame is one
// complete MIDI message.
import { BaseLink } from "./link.ts";

class WsLink extends BaseLink {
  readonly kind = "websocket" as const;
  readonly name: string;
  readonly key: string;
  private readonly ws: WebSocket;
  constructor(ws: WebSocket, name: string, url: string) {
    super();
    this.ws = ws;
    this.name = name;
    this.key = `websocket:${url}`;
    this.reopen = () => openWebSocket(url);
    ws.binaryType = "arraybuffer";
    ws.onmessage = (e) => {
      if (e.data instanceof ArrayBuffer) this.emit(new Uint8Array(e.data));
    };
    ws.onclose = () => this.closed();
  }
  reopen: () => Promise<WsLink>;
  send(msg: Uint8Array): void {
    if (this.ws.readyState === WebSocket.OPEN) this.ws.send(msg);
  }
  close(): void {
    this.ws.close();
  }
}

export function openWebSocket(url: string): Promise<WsLink> {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    ws.onopen = () => resolve(new WsLink(ws, `Simulator (${url})`, url));
    ws.onerror = () => reject(new Error(`cannot connect to ${url}`));
  });
}
