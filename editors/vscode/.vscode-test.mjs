import { defineConfig } from "@vscode/test-cli";
import * as fs from "fs";
import * as os from "os";
import * as path from "path";
import { fileURLToPath } from "url";

const extensionRoot = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(extensionRoot, "../..");

// Each fixture, a copy of a case under .cache/editors, becomes one test run
// with that workspace. `pixi run -e editor editor-prepare` makes the copies.
// Keep in sync with the editor tasks in pixi.toml.
const fixturesDir = path.join(repoRoot, ".cache", "editors");
const fixtures = ["hello_world", "imported_symbol", "two_hosts"];

// The fixtures' databases name a bare clang++, which the server resolves on
// the PATH the task set: xclang's. VS Code would replace that PATH with the
// login shell's, which macOS's path_helper starts with /usr/bin, and Apple's
// clang++ has no C++20 modules.
const keepEnv = "--force-disable-user-env";

// The folders the tests make, and the server staged below, go when this
// process exits, once VS Code has: Windows removes nothing in use, VS Code
// watches the folder of a document outside the workspace a while after its
// editor closes, and a runner's own process watching the programs it saw run
// holds the staged server as long as VS Code runs.
const scratch = fs.mkdtempSync(path.join(os.tmpdir(), "clice-e2e-"));
const staged = path.join(extensionRoot, "clice");
process.on("exit", () => {
    fs.rmSync(scratch, { recursive: true, force: true });
    fs.rmSync(staged, { recursive: true, force: true });
});

const configs = fixtures.map((fixture) => ({
    label: fixture,
    files: "out/test/**/*.test.js",
    workspaceFolder: path.join(fixturesDir, fixture),
    mocha: { timeout: 300 * 1000, failZero: true },
    env: { CLICE_E2E_SCRATCH: scratch },
    launchArgs: [keepEnv],
}));

// Stage the real server under clice/ so it looks like a platform-specific vsix
// (bin/clice + lib/clang resolved relative to the binary). Done here, before VS
// Code launches, because the extension activates on startup and would take the
// missing-bundle path if clice/ were staged only from inside the test.
function stageBundledServer(from) {
    fs.rmSync(staged, { recursive: true, force: true });

    const binDir = path.join(staged, "bin");
    fs.mkdirSync(binDir, { recursive: true });
    const name = process.platform === "win32" ? "clice.exe" : "clice";
    fs.copyFileSync(from, path.join(binDir, name));
    if (process.platform !== "win32") {
        fs.chmodSync(path.join(binDir, name), 0o755);
    }

    fs.cpSync(
        path.resolve(path.dirname(from), "..", "lib", "clang"),
        path.join(staged, "lib", "clang"),
        {
            recursive: true,
        },
    );
}

// Bundled-server variant: CLICE_EXECUTABLE is cleared so the extension takes the
// clice/ fallback; a dedicated user-data-dir isolates it from the clice.executable
// setting the other variants write. CLICE_E2E_BUNDLED_FROM flags the variant for
// the test.
if (process.env.CLICE_EXECUTABLE) {
    stageBundledServer(process.env.CLICE_EXECUTABLE);
    configs.push({
        label: "bundled-hello_world",
        files: "out/test/**/*.test.js",
        workspaceFolder: path.join(fixturesDir, "hello_world"),
        mocha: { timeout: 300 * 1000, failZero: true },
        env: {
            CLICE_EXECUTABLE: "",
            CLICE_E2E_BUNDLED_FROM: process.env.CLICE_EXECUTABLE,
            CLICE_E2E_SCRATCH: scratch,
        },
        launchArgs: [
            keepEnv,
            "--user-data-dir",
            path.join(extensionRoot, ".vscode-test", "user-data-bundled"),
        ],
    });
}

export default defineConfig(configs);
