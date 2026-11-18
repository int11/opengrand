// Web MIDI transport — USB-MIDI and OS-paired BLE-MIDI in Chrome/Edge/Android WebView.
import { BaseLink, type LinkCandidate } from "./link.ts";

class WebMidiLink extends BaseLink {
  readonly kind = "webmidi" as const;
  readonly name: string;
  readonly key: string;
  private readonly input: MIDIInput;
  private readonly output: MIDIOutput;
  constructor(input: MIDIInput, output: MIDIOutput) {
    super();
    this.input = input;
    this.output = output;
    this.name = output.name ?? input.name ?? "MIDI";
    this.key = `webmidi:${output.id}`;
    input.onmidimessage = (e: MIDIMessageEvent) => {
      if (e.data) this.emit(new Uint8Array(e.data));
    };
    input.onstatechange = () => {
      if (input.state === "disconnected") this.closed();
    };
  }
  send(msg: Uint8Array): void {
    this.output.send(msg);
  }
  reopen = async (): Promise<WebMidiLink> => {
    const c = (await listWebMidi()).find((x) => `webmidi:${x.id}` === this.key);
    if (!c) throw new Error(`${this.name} is not available`);
    return (await c.open()) as WebMidiLink;
  };
  close(): void {
    this.input.onmidimessage = null;
    void this.input.close();
    void this.output.close();
    this.closed();
  }
}

/** Lists input/output pairs that share a name (how USB-MIDI devices appear). */
export async function listWebMidi(): Promise<LinkCandidate[]> {
  if (typeof navigator === "undefined" || !("requestMIDIAccess" in navigator)) return [];
  const access = await navigator.requestMIDIAccess({ sysex: true });
  const out: LinkCandidate[] = [];
  for (const output of access.outputs.values()) {
    const input = [...access.inputs.values()].find((i) => i.name === output.name);
    if (!input) continue;
    out.push({
      kind: "webmidi",
      id: output.id,
      key: `webmidi:${output.id}`,
      name: output.name ?? "MIDI",
      open: async () => new WebMidiLink(input, output),
    });
  }
  // Prefer devices that look like a Kawai instrument.
  return out.sort((a, b) => Number(/kawai|ca\d\d/i.test(b.name)) - Number(/kawai|ca\d\d/i.test(a.name)));
}
