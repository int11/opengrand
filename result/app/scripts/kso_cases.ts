// Print the KSO converter test cases as JSON for result/stockfw/tools/stock_kso.py: [{ name, kso (hex) }].
import { smfToKso } from "../src/protocol/kso.ts";
import { KSO_CASES } from "../tests/fixtures/kso_cases.ts";

const hex = (b: Uint8Array) => Buffer.from(b).toString("hex");
console.log(JSON.stringify(KSO_CASES.map((c) => ({ name: c.name, kso: hex(smfToKso(c.smf, c.opts).kso) }))));
