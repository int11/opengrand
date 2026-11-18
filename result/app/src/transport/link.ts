// Common transport interface. Each link carries complete MIDI messages.

export type LinkKind = "webmidi" | "ble" | "android" | "kwmcore" | "websocket" | "loopback";

export interface MidiLink {
  readonly kind: LinkKind;
  readonly name: string;
  /**
   * The endpoint this link talks to (`ble:<device id>`, `android:<shell id>`, `webmidi:<port id>` …).
   * Two links with the same key reach the same piano port: the connection manager never opens a
   * second session on one (src/state/connection.ts).
   */
  readonly key?: string;
  /** Open the same endpoint again without asking the user (after the app let it go). */
  reopen?(): Promise<MidiLink>;
  send(msg: Uint8Array): void;
  /** Register the receive handler (one per link). */
  onMessage(handler: (msg: Uint8Array) => void): void;
  onClose(handler: () => void): void;
  close(): void;
}

export interface LinkCandidate {
  kind: LinkKind;
  id: string;
  name: string;
  /** Same as the opened link's `key`, known before opening (lets a re-pick skip the open). */
  key?: string;
  open(): Promise<MidiLink>;
}

/** Base class handling handler registration; subclasses call emit()/closed(). */
export abstract class BaseLink implements MidiLink {
  abstract readonly kind: LinkKind;
  abstract readonly name: string;
  private handler: ((msg: Uint8Array) => void) | null = null;
  private closeHandlers: (() => void)[] = [];
  abstract send(msg: Uint8Array): void;
  abstract close(): void;
  onMessage(handler: (msg: Uint8Array) => void): void {
    this.handler = handler;
  }
  onClose(handler: () => void): void {
    this.closeHandlers.push(handler);
  }
  protected emit(msg: Uint8Array): void {
    this.handler?.(msg);
  }
  protected closed(): void {
    for (const h of this.closeHandlers) h();
    this.closeHandlers = [];
  }
}

/** In-memory pair, used by tests and the offline demo. */
export class LoopbackLink extends BaseLink {
  readonly kind = "loopback" as const;
  readonly name: string;
  peer: LoopbackLink | null = null;
  constructor(name = "loopback") {
    super();
    this.name = name;
  }
  static pair(): [LoopbackLink, LoopbackLink] {
    const a = new LoopbackLink("a");
    const b = new LoopbackLink("b");
    a.peer = b;
    b.peer = a;
    return [a, b];
  }
  send(msg: Uint8Array): void {
    const p = this.peer;
    if (p) queueMicrotask(() => p.emit(msg));
  }
  close(): void {
    this.closed();
  }
}
