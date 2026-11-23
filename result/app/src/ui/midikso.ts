// A MIDI file on the USB memory into a piano song (「피아노에 넣기」 on a MID row): the piano takes only its own KSO
// into a recorder song (24 09 answers 0xF0 for a MID), so the app reads the MID (24 04), converts it
// (protocol/kso.ts smfToKso), checks the result (checkKso), writes it next to the MID as <name>.KSO (24 05) and reads
// that file back to compare - the caller then imports it with 24 09 <song> 03.
// A KSO the piano did not write is never imported unless checkKso passes: a bad one can make the piano format all
// ten songs at its next power-on.
import { checkKso, KsoError, smfToKso } from "../protocol/kso.ts";
import { freeName } from "./audiorec.ts";

/** English source strings for the converter's refusals (translated by the caller). */
export const KSO_TEXT: Record<KsoError["code"], string> = {
  smf: "This is not a MIDI file the app can read.",
  empty: "The file has no notes.",
  size: "The song is too long for a piano song.",
  kso: "The converted song did not pass the safety check. Nothing was sent to the piano.",
};

export interface MidKsoFiles {
  read(name: string): Promise<Uint8Array>;
  write(name: string, data: Uint8Array): Promise<unknown>;
  delete(name: string): Promise<unknown>;
}

/** Convert `name` (a MID on the USB memory) and write the KSO under a name no file has; throws KsoError. */
export async function midToKsoFile(fa: MidKsoFiles, name: string, files: readonly string[]): Promise<{ name: string; warnings: string[] }> {
  const smf = await fa.read(name);
  const dot = name.lastIndexOf(".");
  const base = dot > name.lastIndexOf("\\") ? name.slice(0, dot) : name;
  const r = smfToKso(smf, { name: base.slice(base.lastIndexOf("\\") + 1) });
  const bad = checkKso(r.kso);
  if (bad) throw new KsoError("kso", bad);
  const out = freeName(files, base, "KSO");
  await fa.write(out, r.kso);
  // what is imported is the file on the USB memory, not these bytes: read it back and compare
  const back = await fa.read(out);
  if (back.length !== r.kso.length || back.some((b, i) => b !== r.kso[i]) || checkKso(back)) {
    await fa.delete(out).catch(() => {});
    throw new KsoError("kso", "the file on the USB memory differs from what was written");
  }
  return { name: out, warnings: r.warnings };
}
