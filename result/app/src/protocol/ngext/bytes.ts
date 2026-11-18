// Little-endian readers shared by the ngext decoders (not re-exported by protocol/ngext.ts).
export const u16 = (b: Uint8Array, o: number) => b[o]! | (b[o + 1]! << 8);
export const u32 = (b: Uint8Array, o: number) => (b[o]! | (b[o + 1]! << 8) | (b[o + 2]! << 16) | (b[o + 3]! << 24)) >>> 0;
