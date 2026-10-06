/// Runs the translation checker, @clice-io/translate from clice-io/docs,
/// at the release the CI docs check pins: a sparse checkout of it under
/// .cache/, dependencies installed once. Stands in until the package is on
/// npm and the pixi tasks call `npx @clice-io/translate@1` directly.

import { spawnSync, type SpawnSyncOptions } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { REPO_ROOT } from "../compile_commands.ts";

const RELEASE = "8038e6acbd499dc2376392ca610faca66b2deddf";

const checkout = path.join(REPO_ROOT, ".cache", `clice-docs-${RELEASE}`);
const tool = path.join(checkout, "tools", "translations");

function run(command: string, args: string[], options: SpawnSyncOptions = {}): void {
    const result = spawnSync(command, args, { cwd: checkout, stdio: "inherit", ...options });
    if (result.status !== 0) {
        console.error(`${command} ${args.join(" ")} failed`);
        process.exit(1);
    }
}

if (!fs.existsSync(path.join(tool, "node_modules"))) {
    fs.rmSync(checkout, { recursive: true, force: true });
    fs.mkdirSync(checkout, { recursive: true });
    run("git", ["init", "--quiet"]);
    run("git", ["remote", "add", "origin", "https://github.com/clice-io/docs.git"]);
    run("git", ["sparse-checkout", "set", "tools/translations"]);
    run("git", ["fetch", "--quiet", "--depth=1", "--filter=blob:none", "origin", RELEASE]);
    run("git", ["checkout", "--quiet", "FETCH_HEAD"]);
    // A command line rather than arguments: npm is a .cmd shim on Windows,
    // which only a shell starts.
    run("npm ci --omit=dev --ignore-scripts --no-audit --no-fund --silent", [], {
        cwd: tool,
        shell: true,
    });
}

const cli = path.join(tool, "src", "cli.ts");
const result = spawnSync(process.execPath, [cli, ...process.argv.slice(2)], {
    cwd: REPO_ROOT,
    stdio: "inherit",
});
process.exit(result.status ?? 1);
