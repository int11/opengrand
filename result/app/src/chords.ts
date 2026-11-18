// Chord dictionary (feature 80). Chords are generated from interval formulas
// instead of a stored table: root (12) x quality (15) x inversion.
// Display only — playing the chord on the piano uses ordinary note messages.

export const NOTE_NAMES = ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"] as const;

export interface ChordQuality {
  id: string;
  symbol: string; // suffix after the root, e.g. "m7"
  intervals: number[]; // semitones above the root, ascending
}

export const QUALITIES: ChordQuality[] = [
  { id: "maj", symbol: "", intervals: [0, 4, 7] },
  { id: "min", symbol: "m", intervals: [0, 3, 7] },
  { id: "7", symbol: "7", intervals: [0, 4, 7, 10] },
  { id: "maj7", symbol: "maj7", intervals: [0, 4, 7, 11] },
  { id: "m7", symbol: "m7", intervals: [0, 3, 7, 10] },
  { id: "dim", symbol: "dim", intervals: [0, 3, 6] },
  { id: "aug", symbol: "aug", intervals: [0, 4, 8] },
  { id: "sus4", symbol: "sus4", intervals: [0, 5, 7] },
  { id: "sus2", symbol: "sus2", intervals: [0, 2, 7] },
  { id: "add9", symbol: "add9", intervals: [0, 4, 7, 14] },
  { id: "6", symbol: "6", intervals: [0, 4, 7, 9] },
  { id: "m6", symbol: "m6", intervals: [0, 3, 7, 9] },
  { id: "9", symbol: "9", intervals: [0, 4, 7, 10, 14] },
  { id: "m7b5", symbol: "m7b5", intervals: [0, 3, 6, 10] },
  { id: "dim7", symbol: "dim7", intervals: [0, 3, 6, 9] },
];

export interface Chord {
  name: string; // e.g. "Cmaj7" or "C/E" style for inversions: "Cmaj7 (1st inv.)"
  root: number; // pitch class 0..11
  quality: ChordQuality;
  inversion: number;
  notes: number[]; // MIDI note numbers, ascending
}

export function chordName(root: number, q: ChordQuality): string {
  return `${NOTE_NAMES[((root % 12) + 12) % 12]}${q.symbol}`;
}

/** Number of inversions a quality supports (root position + n-1). */
export function inversionCount(q: ChordQuality): number {
  return q.intervals.length;
}

/**
 * Build a chord. `base` is the MIDI note of the root in root position
 * (default C4 = 60 shifted by the root). Inversion k moves the k lowest notes
 * up an octave.
 */
export function buildChord(root: number, qualityId: string, inversion = 0, baseOctaveNote = 60): Chord {
  const q = QUALITIES.find((x) => x.id === qualityId);
  if (!q) throw new RangeError(`unknown chord quality ${qualityId}`);
  const pc = ((root % 12) + 12) % 12;
  const inv = ((inversion % q.intervals.length) + q.intervals.length) % q.intervals.length;
  const notes = q.intervals.map((i) => baseOctaveNote + pc + i);
  for (let k = 0; k < inv; k++) notes.push(notes.shift()! + 12);
  // keep the voicing within the 88 keys and as close to middle C as possible
  while (notes[0] > 66) for (let i = 0; i < notes.length; i++) notes[i] -= 12;
  return { name: chordName(pc, q), root: pc, quality: q, inversion: inv, notes };
}

/** All root x quality combinations (180 chords, root position). */
export function allChords(): Chord[] {
  const out: Chord[] = [];
  for (let r = 0; r < 12; r++) for (const q of QUALITIES) out.push(buildChord(r, q.id));
  return out;
}

/**
 * Identify a chord from held notes (any octave, any inversion). Returns the
 * chord name with a slash bass when the lowest note is not the root.
 */
export function identifyChord(held: number[]): string | null {
  if (held.length < 3) return null;
  const sorted = [...new Set(held)].sort((a, b) => a - b);
  const pcs = [...new Set(sorted.map((n) => n % 12))];
  const bass = sorted[0] % 12;
  let slash: string | null = null;
  // prefer a reading whose root is the bass note (C6 rather than Am7/C)
  for (const root of [bass, ...pcs.filter((p) => p !== bass)]) {
    const rel = pcs.map((p) => (p - root + 12) % 12).sort((a, b) => a - b);
    for (const q of QUALITIES) {
      const shape = [...new Set(q.intervals.map((i) => i % 12))].sort((a, b) => a - b);
      if (shape.length !== rel.length || !rel.every((v, i) => v === shape[i])) continue;
      const name = chordName(root, q);
      if (root === bass) return name;
      slash ??= `${name}/${NOTE_NAMES[bass]}`;
    }
  }
  return slash;
}
