/// Runs the translation checker, @clice-io/translate from clice-io/docs,
/// at the release the lint workflow's docs job pins, so a local run and CI
/// run the same checker: a sparse checkout of it under .cache/ with its
/// dependencies installed. Stands in until the package is on npm and the
/// pixi tasks call `npx @clice-io/translate@1` directly.

import { spawnSync, type SpawnSyncOptions } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { REPO_ROOT } from "../compile_commands.ts";

function fail(message: string): never {
    console.error(message);
    process.exit(1);
}

function run(command: string, args: string[], options: SpawnSyncOptions = {}): void {
    const result = spawnSync(command, args, { stdio: "inherit", ...options });
    if (result.status !== 0) {
        fail(`${command} ${args.join(" ")} failed`);
    }
}

const workflow = fs.readFileSync(path.join(REPO_ROOT, ".github/workflows/lint.yml"), "utf8");
const release =
    /uses: clice-io\/docs\/check-translations@(\S+)/.exec(workflow)?.[1] ??
    fail("lint.yml does not run clice-io/docs/check-translations");
const checkout = path.join(REPO_ROOT, ".cache", `clice-docs-${release}`);

if (!fs.existsSync(checkout)) {
    // Built aside and renamed into place, so a checkout that exists is
    // complete even when an install was interrupted or raced by another.
    const staging = `${checkout}.${process.pid}`;
    fs.rmSync(staging, { recursive: true, force: true });
    fs.mkdirSync(staging, { recursive: true });
    const git = (...args: string[]) => {
        run("git", args, { cwd: staging });
    };
    git("init", "--quiet");
    git("remote", "add", "origin", "https://github.com/clice-io/docs.git");
    git("sparse-checkout", "set", "tools/translations");
    git("fetch", "--quiet", "--depth=1", "--filter=blob:none", "origin", release);
    git("checkout", "--quiet", "FETCH_HEAD");
    // A command line rather than arguments: npm is a .cmd shim on Windows,
    // which only a shell starts.
    run("npm ci --omit=dev --ignore-scripts --no-audit --no-fund --silent", [], {
        cwd: path.join(staging, "tools", "translations"),
        shell: true,
    });
    try {
        fs.renameSync(staging, checkout);
    } catch (error) {
        if (!fs.existsSync(checkout)) {
            throw error;
        }
        fs.rmSync(staging, { recursive: true, force: true });
    }
}

const cli = path.join(checkout, "tools", "translations", "src", "cli.ts");
const result = spawnSync(process.execPath, [cli, ...process.argv.slice(2)], {
    cwd: REPO_ROOT,
    stdio: "inherit",
});
process.exit(result.status ?? 1);
