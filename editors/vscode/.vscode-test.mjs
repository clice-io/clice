import { defineConfig } from "@vscode/test-cli";
import * as fs from "fs";
import * as path from "path";
import { fileURLToPath } from "url";

const extensionRoot = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(extensionRoot, "../..");

// Each workspace under build/e2e becomes one test run; pixi's editor-prepare
// copies them there from samples/. Keep in sync with the editor tasks in
// pixi.toml.
const workspaces = ["tiny", "modules", "contexts"];

const configs = workspaces.map((workspace) => ({
    label: workspace,
    files: "out/test/**/*.test.js",
    workspaceFolder: path.join(repoRoot, "build/e2e", workspace),
    mocha: { timeout: 300 * 1000 },
}));

// Stage the real server under clice/ so it looks like a platform-specific vsix
// (bin/clice + lib/clang resolved relative to the binary). Done here, before VS
// Code launches, because the extension activates on startup and would take the
// missing-bundle path if clice/ were staged only from inside the test.
function stageBundledServer(from) {
    const dir = path.join(extensionRoot, "clice");
    fs.rmSync(dir, { recursive: true, force: true });

    const binDir = path.join(dir, "bin");
    fs.mkdirSync(binDir, { recursive: true });
    const name = process.platform === "win32" ? "clice.exe" : "clice";
    fs.copyFileSync(from, path.join(binDir, name));
    if (process.platform !== "win32") {
        fs.chmodSync(path.join(binDir, name), 0o755);
    }

    fs.cpSync(
        path.resolve(path.dirname(from), "..", "lib", "clang"),
        path.join(dir, "lib", "clang"),
        {
            recursive: true,
        },
    );
}

// Bundled-server variant: CLICE_EXECUTABLE is cleared so the extension takes the
// clice/ fallback; a dedicated user-data-dir isolates it from the clice.executable
// setting the other variants write. CLICE_E2E_BUNDLED_FROM flags the variant for
// the test (which clears the staged dir on teardown).
if (process.env.CLICE_EXECUTABLE) {
    stageBundledServer(process.env.CLICE_EXECUTABLE);
    configs.push({
        label: "bundled-tiny",
        files: "out/test/**/*.test.js",
        workspaceFolder: path.join(repoRoot, "build/e2e", "tiny"),
        mocha: { timeout: 300 * 1000 },
        env: {
            CLICE_EXECUTABLE: "",
            CLICE_E2E_BUNDLED_FROM: process.env.CLICE_EXECUTABLE,
        },
        launchArgs: [
            "--user-data-dir",
            path.join(extensionRoot, ".vscode-test", "user-data-bundled"),
        ],
    });
}

export default defineConfig(configs);
