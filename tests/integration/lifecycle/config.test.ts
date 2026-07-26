/// Integration tests for clice configuration (clice.toml + initializationOptions).
///
/// Each workspace's main.cpp references a macro that is only defined when the
/// rule's `-D<macro>=...` is applied. When rules are applied, compilation is
/// clean; otherwise an undeclared-identifier diagnostic surfaces.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import * as proto from "vscode-languageserver-protocol";
import {
    assertCleanCompile,
    assertHasErrors,
    assertNoAnomaly,
    getErrors,
} from "../../tools/checks.ts";
import { cliceExecutable, expect, test } from "../../tools/fixtures.ts";
import { makeClient, shutdownClient } from "../../tools/lifecycle.ts";

function messageText(d: proto.Diagnostic): string {
    return typeof d.message === "string" ? d.message : d.message.value;
}

test("baseline without rules", async ({ session }) => {
    const { client, workspace } = await session("config_rules_no_config");
    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertHasErrors(client, uri, "Expected diagnostics without any rules applied");
    const errors = getErrors(client.diagnostics.get(uri) ?? []);
    expect(
        errors.some((d) => messageText(d).includes("FROM_INIT")),
        `Expected a diagnostic referencing FROM_INIT, got: ${JSON.stringify(errors)}`,
    ).toBe(true);
});

test("rules from toml", async ({ session }) => {
    const { client, workspace } = await session("config_rules_toml");
    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    const symbols = await client.documentSymbols(uri);
    expect(symbols && symbols.length > 0, "Expected document symbols for value()/main()").toBe(
        true,
    );
    const hover = await client.hoverAt(uri, 4, 4); // on 'main'
    expect(hover).not.toBeNull();
});

test("rules from init options", async ({ session }) => {
    const { client, workspace } = await session("config_rules_no_config", {
        initializationOptions: { rules: [{ patterns: ["**/*.cpp"], append: ["-DFROM_INIT=1"] }] },
    });
    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);
});

test("init options replaces toml rules", async ({ session }) => {
    const { client, workspace } = await session("config_rules_toml", {
        initializationOptions: { rules: [{ patterns: ["**/*.cpp"], append: ["-DUNRELATED"] }] },
    });
    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertHasErrors(client, uri, "initializationOptions should have overridden clice.toml rules");
    const errors = getErrors(client.diagnostics.get(uri) ?? []);
    expect(
        errors.some((d) => messageText(d).includes("FROM_TOML")),
        `Expected FROM_TOML diagnostic after override, got: ${JSON.stringify(errors)}`,
    ).toBe(true);
});

test("rules pattern mismatch", async ({ session }) => {
    const { client, workspace } = await session("config_rules_no_config", {
        initializationOptions: {
            rules: [{ patterns: ["**/does_not_match.cpp"], append: ["-DFROM_INIT=1"] }],
        },
    });
    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertHasErrors(client, uri, "Rule pattern should not have matched main.cpp");
});

function mkTmp(): string {
    return fs.mkdtempSync(path.join(os.tmpdir(), "clice-test-"));
}

test("config type error diagnostic", async () => {
    // Wrong value type → Error diagnostic on the clice.toml URI; the config
    // falls back to defaults. (Line/column pinpointing awaits the kotatsu
    // TOML error-location feature — see config_tests.cpp.)
    const workspace = mkTmp();
    try {
        fs.writeFileSync(path.join(workspace, "clice.toml"), '[project]\nclang_tidy = "yes"\n');
        fs.writeFileSync(path.join(workspace, "main.cpp"), "int main() { return 0; }\n");
        const client = await makeClient(cliceExecutable(), workspace);
        try {
            const tomlUri = client.pathToUri(path.join(workspace, "clice.toml"));
            await client.waitDiagnostics(tomlUri, 10_000);
            const diags = client.diagnostics.get(tomlUri) ?? [];
            expect(diags.length, `expected one config diagnostic: ${JSON.stringify(diags)}`).toBe(
                1,
            );
            expect(diags[0]!.severity).toBe(proto.DiagnosticSeverity.Error);
            expect(diags[0]!.message).toContain("clang_tidy");
            assertNoAnomaly(client, workspace);
        } finally {
            await shutdownClient(client);
        }
    } finally {
        fs.rmSync(workspace, { recursive: true, force: true });
    }
});

test("config unknown key diagnostic", async () => {
    // Typo'd key → Warning diagnostic; the rest of the config still applies.
    const workspace = mkTmp();
    try {
        fs.writeFileSync(path.join(workspace, "clice.toml"), "[project]\nclang_tdy = true\n");
        fs.writeFileSync(path.join(workspace, "main.cpp"), "int main() { return 0; }\n");
        const client = await makeClient(cliceExecutable(), workspace);
        try {
            const tomlUri = client.pathToUri(path.join(workspace, "clice.toml"));
            await client.waitDiagnostics(tomlUri, 10_000);
            const diags = client.diagnostics.get(tomlUri) ?? [];
            expect(diags.length, `expected one config diagnostic: ${JSON.stringify(diags)}`).toBe(
                1,
            );
            expect(diags[0]!.severity).toBe(proto.DiagnosticSeverity.Warning);
            expect(diags[0]!.message).toContain("clang_tdy");
            assertNoAnomaly(client, workspace);
        } finally {
            await shutdownClient(client);
        }
    } finally {
        fs.rmSync(workspace, { recursive: true, force: true });
    }
});

test("config diagnostic clears after fix", async () => {
    const workspace = mkTmp();
    try {
        fs.writeFileSync(path.join(workspace, "clice.toml"), '[project]\nclang_tidy = "yes"\n');
        fs.writeFileSync(path.join(workspace, "main.cpp"), "int main() { return 0; }\n");
        let client = await makeClient(cliceExecutable(), workspace);
        try {
            const tomlUri = client.pathToUri(path.join(workspace, "clice.toml"));
            await client.waitDiagnostics(tomlUri, 10_000);
            expect(
                (client.diagnostics.get(tomlUri) ?? []).length,
                "broken config should be diagnosed",
            ).toBeGreaterThan(0);
            assertNoAnomaly(client, workspace);
        } finally {
            await shutdownClient(client);
        }

        // Fix the config and restart — the new session publishes an empty list
        // for the config URI so stale markers clear.
        fs.writeFileSync(path.join(workspace, "clice.toml"), "[project]\nclang_tidy = true\n");
        client = await makeClient(cliceExecutable(), workspace);
        try {
            const tomlUri = client.pathToUri(path.join(workspace, "clice.toml"));
            await client.waitDiagnostics(tomlUri, 10_000);
            expect(client.diagnostics.get(tomlUri), "fixed config must clear diagnostics").toEqual(
                [],
            );
            assertNoAnomaly(client, workspace);
        } finally {
            await shutdownClient(client);
        }
    } finally {
        fs.rmSync(workspace, { recursive: true, force: true });
    }
});

test("config dump logged", async () => {
    // The startup log is the discoverable record of the resolved paths.
    const workspace = mkTmp();
    try {
        const client = await makeClient(cliceExecutable(), workspace);
        try {
            assertNoAnomaly(client, workspace);
        } finally {
            await shutdownClient(client);
        }

        const logsDir = path.join(workspace, ".clice", "logs");
        const names = fs
            .readdirSync(logsDir, { recursive: true, encoding: "utf8" })
            .filter((name) => path.basename(name) === "master.log");
        expect(names.length, "expected a master.log").toBeGreaterThan(0);
        const text = names
            .map((name) => fs.readFileSync(path.join(logsDir, name), "utf8"))
            .join("");
        expect(text).toContain("Session log directory:");
        // All three config layers are dumped: file, overlay, merged result.
        expect(text).toContain("Configuration file");
        expect(text).toContain("initializationOptions");
        expect(text).toContain("Effective configuration:");
        expect(text).toContain('"cache_dir"');
    } finally {
        fs.rmSync(workspace, { recursive: true, force: true });
    }
});
