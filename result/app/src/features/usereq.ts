// User tone control (4-band EQ) codec. Wire (protocol table, stock codec classes for UserEqGain / UserEqFreq):
//   UserEqGain 51 1A 7F [band, gain + 64, 0]           band 0 Low, 1 Mid-low, 2 Mid-high, 3 High
//   UserEqFreq 51 1B 7F [band, semitone + 9, frac + 64] semitones above 27.5 Hz, frac in 1/128 semitone;
//                                                       only the two mid bands have a frequency
// The stock editor offers gain -6..+6 dB and a fixed list of centre frequencies from 200 Hz to 8 kHz.
// Check values: the piano's own defaults `01 3D 56` = 560 Hz and `02 4B 4A` = 1250 Hz (dump captures).

export const EQ_BANDS = ["Low", "Mid-low", "Mid-high", "High"] as const;
export const EQ_FREQ_BANDS = [1, 2] as const;
export const EQ_GAIN_MIN = -6;
export const EQ_GAIN_MAX = 6;

/** Centre frequencies offered for the mid bands (Hz). */
export const EQ_FREQS = [
  200, 224, 250, 280, 315, 355, 400, 450, 500, 560, 630, 710, 800, 900, 1000, 1120, 1250, 1400, 1600, 1800,
  2000, 2240, 2500, 2800, 3150, 3500, 4000, 4500, 5000, 5600, 6300, 7100, 8000,
];

export const eqGain = {
  encode: (band: number, db: number): number[] => [band, Math.max(0, Math.min(127, Math.round(db) + 64)), 0],
  decode: (d: readonly number[]): number => (d[1] ?? 64) - 64,
};

export const eqFreq = {
  encode: (band: number, hz: number): number[] => {
    const x = 12 * Math.log2(hz / 27.5);
    const semi = Math.round(x);
    return [band, semi + 9, Math.round(128 * (x - semi)) + 64];
  },
  decode: (d: readonly number[]): number => Math.round(27.5 * 2 ** (((d[1] ?? 9) - 9 + ((d[2] ?? 64) - 64) / 128) / 12)),
};

/** Index of the offered frequency nearest to `hz`. */
export function nearestFreq(hz: number): number {
  let best = 0;
  EQ_FREQS.forEach((f, i) => {
    if (Math.abs(f - hz) < Math.abs(EQ_FREQS[best] - hz)) best = i;
  });
  return best;
}

export function formatHz(hz: number): string {
  return hz >= 1000 ? `${+(hz / 1000).toFixed(2)} kHz` : `${hz} Hz`;
}
