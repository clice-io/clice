import { defineConfig } from "@vscode/test-cli";
import * as path from "path";
import { fileURLToPath } from "url";

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");

// Each fixture under tests/data becomes one test run with that workspace.
// Run tests/prepare.py first to generate compile databases.
// Keep in sync with the editor tasks in pixi.toml.
const fixtures = ["hello_world", "modules/hover_on_imported_symbol"];

export default defineConfig(
    fixtures.map((fixture) => ({
        label: fixture,
        files: "out/test/**/*.test.js",
        workspaceFolder: path.join(repoRoot, "tests/data", fixture),
        mocha: { timeout: 300 * 1000 },
    })),
);
