import { defineConfig } from "@vscode/test-cli";
import * as fs from "fs";
import * as os from "os";
import * as path from "path";
import { fileURLToPath } from "url";

const extensionRoot = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(extensionRoot, "../..");

// With CLICE_EXECUTABLE, each fixture, a copy of tests/editors/NN_name under
// .cache/editors/vscode (`pixi run -e editor editor-prepare vscode`), becomes
// one run of the end-to-end tests with that workspace, which its
// scenario.json drives.
const fixturesDir = path.join(repoRoot, ".cache", "editors", "vscode");
const fixtures = [];
if (process.env.CLICE_EXECUTABLE) {
    const names = fs.existsSync(fixturesDir) ? fs.readdirSync(fixturesDir) : [];
    fixtures.push(
        ...names
            .filter((name) => fs.existsSync(path.join(fixturesDir, name, "scenario.json")))
            .sort(),
    );
    if (fixtures.length === 0) {
        throw new Error(
            `no fixtures under ${fixturesDir}: pixi run -e editor editor-prepare vscode`,
        );
    }
}

// The fixtures' databases name a bare clang++, which the server resolves on
// the PATH the task set: xclang's. VS Code would replace that PATH with the
// login shell's, which macOS's path_helper starts with /usr/bin, and Apple's
// clang++ has no C++20 modules.
const keepEnv = "--force-disable-user-env";

// The folders the tests make, and the server staged below, go when this
// process exits, once VS Code has: Windows removes nothing in use, VS Code
// watches the folder of a document outside the workspace a while after its
// editor closes, and a runner's own process watching the programs it saw run
// holds the staged server as long as VS Code runs. The server's workers may
// outlive VS Code by a moment, holding it still: what cannot go yet stays,
// said so, and the next run's staging replaces it.
const scratch = fs.mkdtempSync(path.join(os.tmpdir(), "clice-e2e-"));
const staged = path.join(extensionRoot, "clice");
process.on("exit", () => {
    for (const dir of [scratch, staged]) {
        try {
            fs.rmSync(dir, { recursive: true, force: true });
        } catch (error) {
            console.error(`${dir} stays, still in use: ${String(error)}`);
        }
    }
});

const mocha = { timeout: 300 * 1000, failZero: true };

// The extension's own tests, which no fixture changes, run once, in a
// workspace of their own.
const extensionWorkspace = path.join(scratch, "workspace");
fs.mkdirSync(extensionWorkspace);

// The conflict check's tests report clangd and cpptools installed without
// installing them, but VS Code writes only settings an installed extension
// declares: this one declares those the check turns off, as the two do.
const conflictingSettings = path.join(scratch, "conflicting-settings");
fs.mkdirSync(conflictingSettings);
fs.writeFileSync(
    path.join(conflictingSettings, "package.json"),
    JSON.stringify({
        name: "conflicting-settings",
        publisher: "clice-test",
        version: "0.0.0",
        engines: { vscode: "*" },
        contributes: {
            configuration: {
                properties: {
                    "clangd.enable": { type: "boolean", default: true },
                    "C_Cpp.intelliSenseEngine": {
                        type: "string",
                        enum: ["default", "disabled"],
                        default: "default",
                        scope: "resource",
                    },
                },
            },
        },
    }),
);

const configs = [
    {
        label: "extension",
        files: "out/test/extension.test.js",
        extensionDevelopmentPath: [extensionRoot, conflictingSettings],
        workspaceFolder: extensionWorkspace,
        mocha,
        env: { CLICE_E2E_SCRATCH: scratch },
        // A profile of its own, new every run: the conflict check's "Never
        // ask again" is kept in the profile, and the next run must be warned.
        launchArgs: [keepEnv, "--user-data-dir", path.join(scratch, "user-data")],
    },
    ...fixtures.map((fixture) => ({
        label: fixture,
        files: "out/test/e2e.test.js",
        workspaceFolder: path.join(fixturesDir, fixture),
        mocha,
        env: { CLICE_E2E_SCRATCH: scratch },
        launchArgs: [keepEnv],
    })),
];

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

// Bundled-server variant, on the first fixture: CLICE_EXECUTABLE is cleared
// so the extension takes the clice/ fallback; a dedicated user-data-dir
// isolates it from the clice.executable setting the other variants write.
// CLICE_E2E_BUNDLED_FROM flags the variant for the test.
if (process.env.CLICE_EXECUTABLE) {
    stageBundledServer(process.env.CLICE_EXECUTABLE);
    configs.push({
        label: `bundled-${fixtures[0]}`,
        files: "out/test/e2e.test.js",
        workspaceFolder: path.join(fixturesDir, fixtures[0]),
        mocha,
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
