// npm run install:win [-- <dest>] [-- --shortcut]: copy the Windows build (after npm run dist:win) to a Windows folder.
// Default dest: %LOCALAPPDATA%\OpenGrand (resolved through cmd.exe + wslpath when run from WSL); pass <dest> otherwise.
//   <dest>\OpenGrand\OpenGrand.exe          unpacked app (fast start; test tools launch this one)
//   <dest>\OpenGrand-portable.exe         single-file portable build (extracts to %TEMP% on each start)
// --shortcut: also put "OpenGrand" on the Windows desktop, pointing at the unpacked exe (the portable / dir
// targets have no installer that would make one; created through PowerShell + WScript.Shell, from WSL too).
// Fails while OpenGrand.exe is running (Windows locks its files): close the app first.
import { execFileSync } from "node:child_process";
import { cpSync, existsSync, mkdirSync, readdirSync, rmSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const pkg = join(dirname(fileURLToPath(import.meta.url)), "..");
const out = join(pkg, "dist-electron");
const unpacked = join(out, "win-unpacked");
const args = process.argv.slice(2);
const shortcut = args.includes("--shortcut");
// %LOCALAPPDATA%\OpenGrand, or null when it cannot be found (plain Linux, no Windows interop)
function defaultDest() {
  if (process.platform === "win32") return process.env.LOCALAPPDATA ? join(process.env.LOCALAPPDATA, "OpenGrand") : null;
  try {
    const opts = { encoding: "utf8", stdio: ["ignore", "pipe", "ignore"], ...(existsSync("/mnt/c") ? { cwd: "/mnt/c" } : {}) };
    const win = execFileSync("cmd.exe", ["/d", "/c", "echo %LOCALAPPDATA%"], opts).trim();
    if (!win || win.includes("%")) return null;
    return join(execFileSync("wslpath", ["-u", win], { encoding: "utf8" }).trim(), "OpenGrand");
  } catch {
    return null;
  }
}
const dest = args.find((a) => !a.startsWith("--")) || defaultDest();
if (!dest) {
  console.error("cannot resolve %LOCALAPPDATA% - pass the destination: npm run install:win -- <dest>");
  process.exit(1);
}

if (!existsSync(join(unpacked, "OpenGrand.exe"))) {
  console.error(`no Windows build in ${unpacked} - run: npm run dist:win`);
  process.exit(1);
}
mkdirSync(dest, { recursive: true });
const appDir = join(dest, "OpenGrand");
rmSync(appDir, { recursive: true, force: true });
cpSync(unpacked, appDir, { recursive: true });
// the portable exe carries its version in the name (OpenGrand-0.2.0-portable.exe): install the newest one
const portable = readdirSync(out).filter((f) => /-portable\.exe$/.test(f)).sort().pop();
if (portable) cpSync(join(out, portable), join(dest, "OpenGrand-portable.exe"));
console.log(`installed: ${join(appDir, "OpenGrand.exe")}${portable ? ` and ${join(dest, "OpenGrand-portable.exe")}` : ""}`);

if (shortcut) {
  const exe = join(appDir, "OpenGrand.exe");
  const winExe = process.platform === "win32" ? exe : execFileSync("wslpath", ["-w", exe], { encoding: "utf8" }).trim();
  const ps = [
    "[Console]::OutputEncoding = [Text.Encoding]::UTF8",
    "$s = New-Object -ComObject WScript.Shell",
    "$l = $s.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'OpenGrand.lnk'))",
    `$l.TargetPath = '${winExe.replace(/'/g, "''")}'`,
    `$l.WorkingDirectory = '${dirname(winExe.replace(/\\/g, "/")).replace(/\//g, "\\").replace(/'/g, "''")}'`,
    `$l.IconLocation = '${winExe.replace(/'/g, "''")},0'`,
    "$l.Description = 'OpenGrand'",
    "$l.Save()",
    "Write-Output $l.FullName",
  ].join("; ");
  const lnk = execFileSync(process.platform === "win32" ? "powershell" : "powershell.exe", ["-NoProfile", "-NonInteractive", "-Command", ps], { encoding: "utf8" }).trim();
  console.log(`desktop shortcut: ${lnk}`);
}
