// Build a USB update package for feature 86:
//   node scripts/mkupdate.mjs <app.apk> <usb-root> [version] [blemidi.zip blemidi-version]
// writes <usb-root>/CA79NG/app.apk + manifest.json {version, sha256, file}
// and optionally <usb-root>/CA79NG/blemidi.zip + blemidi.json {version} (feature 87).
import { createHash } from "node:crypto";
import { copyFileSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const [apk, root, version, bleZip, bleVersion] = process.argv.slice(2);
if (!apk || !root) {
  console.error("usage: node scripts/mkupdate.mjs <app.apk> <usb-root> [version] [blemidi.zip blemidi-version]");
  process.exit(2);
}
const ver = version ?? JSON.parse(readFileSync("package.json", "utf8")).version;
if (!/^\d+(\.\d+)*$/.test(ver)) throw new Error(`bad version ${ver}`);
const dir = join(root, "CA79NG");
mkdirSync(dir, { recursive: true });
copyFileSync(apk, join(dir, "app.apk"));
const sha256 = createHash("sha256").update(readFileSync(apk)).digest("hex");
writeFileSync(join(dir, "manifest.json"), JSON.stringify({ version: ver, sha256, file: "app.apk" }, null, 2) + "\n");
if (bleZip) {
  copyFileSync(bleZip, join(dir, "blemidi.zip"));
  writeFileSync(join(dir, "blemidi.json"), JSON.stringify({ version: bleVersion ?? "0" }) + "\n");
}
console.log(`${dir}: app.apk ${ver} sha256=${sha256}${bleZip ? " + blemidi.zip" : ""}`);
