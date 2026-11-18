// npm run desktop: start Electron on this package, passing our flags through.
// Drops ELECTRON_RUN_AS_NODE (VS Code terminals set it; Electron would then run as plain Node).
import { spawn } from "node:child_process";
import { createRequire } from "node:module";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const electron = createRequire(import.meta.url)("electron");
const env = { ...process.env };
delete env.ELECTRON_RUN_AS_NODE;
const pkg = join(dirname(fileURLToPath(import.meta.url)), "..");
const child = spawn(electron, [pkg, ...process.argv.slice(2)], { env, stdio: "inherit" });
child.on("exit", (code, signal) => process.exit(signal ? 1 : (code ?? 0)));
for (const s of ["SIGINT", "SIGTERM"]) process.on(s, () => child.kill(s));
