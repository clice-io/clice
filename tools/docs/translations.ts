/// Runs the translation checker, @clice-io/translate from clice-io/docs,
/// at the commit the CI docs check runs — the one the moving v1 tag names:
/// a sparse checkout of it under .cache/, dependencies installed once.
/// Stands in until the package is on npm and the pixi tasks call
/// `npx @clice-io/translate@1` directly.

import { spawnSync, type SpawnSyncOptions } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { REPO_ROOT } from "../compile_commands.ts";

const REMOTE = "https://github.com/clice-io/docs.git";
const TAG = "v1";

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

/// The commit TAG names; an annotated tag lists twice, the peeled line
/// (`^{}`) naming the commit.
function taggedCommit(): string {
    const result = spawnSync(
        "git",
        ["ls-remote", REMOTE, `refs/tags/${TAG}`, `refs/tags/${TAG}^{}`],
        { encoding: "utf8" },
    );
    if (result.status !== 0) {
        fail(`cannot list ${TAG} of ${REMOTE}: ${result.stderr.trim()}`);
    }
    const refs = result.stdout
        .trim()
        .split("\n")
        .map((line) => line.split("\t"));
    const commit = (refs.find(([, ref]) => ref?.endsWith("^{}")) ?? refs.at(0))?.at(0);
    return commit === undefined || commit === "" ? fail(`${REMOTE} has no tag ${TAG}`) : commit;
}

const commit = taggedCommit();
const cache = path.join(REPO_ROOT, ".cache");
const checkout = path.join(cache, `clice-docs-${commit}`);
const tool = path.join(checkout, "tools", "translations");
// Written once `npm ci` succeeded; a checkout without it is incomplete.
const installed = path.join(checkout, ".installed");

if (!fs.existsSync(installed)) {
    fs.mkdirSync(cache, { recursive: true });
    for (const entry of fs.readdirSync(cache, { withFileTypes: true })) {
        if (entry.name.startsWith("clice-docs-")) {
            fs.rmSync(path.join(cache, entry.name), { recursive: true, force: true });
        }
    }
    fs.mkdirSync(checkout, { recursive: true });
    const git = (...args: string[]) => {
        run("git", args, { cwd: checkout });
    };
    git("init", "--quiet");
    git("remote", "add", "origin", REMOTE);
    git("sparse-checkout", "set", "tools/translations");
    git("fetch", "--quiet", "--depth=1", "--filter=blob:none", "origin", commit);
    git("checkout", "--quiet", "FETCH_HEAD");
    // A command line rather than arguments: npm is a .cmd shim on Windows,
    // which only a shell starts.
    run("npm ci --omit=dev --ignore-scripts --no-audit --no-fund --silent", [], {
        cwd: tool,
        shell: true,
    });
    fs.writeFileSync(installed, "");
}

const cli = path.join(tool, "src", "cli.ts");
const result = spawnSync(process.execPath, [cli, ...process.argv.slice(2)], {
    cwd: REPO_ROOT,
    stdio: "inherit",
});
process.exit(result.status ?? 1);
