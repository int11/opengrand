// Tiny format-0 SMF builder for tests: notes on channel 1 (and 2 if given).
export function tinySmf(notes: number[] = [60, 64, 67], ch2: number[] = []): Uint8Array {
  const ev: number[] = [0x00, 0xff, 0x51, 0x03, 0x07, 0xa1, 0x20]; // tempo 120
  for (const n of notes) ev.push(0x00, 0x90, n, 80, 0x60, 0x80, n, 0);
  for (const n of ch2) ev.push(0x00, 0x91, n, 70, 0x60, 0x81, n, 0);
  ev.push(0x00, 0xff, 0x2f, 0x00);
  const len = ev.length;
  return Uint8Array.from([
    0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xe0,
    0x4d, 0x54, 0x72, 0x6b, (len >> 24) & 255, (len >> 16) & 255, (len >> 8) & 255, len & 255, ...ev,
  ]);
}
