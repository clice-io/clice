/// Integration tests for clice configuration (clice.toml + initializationOptions).
///
/// Each workspace's main.cpp references a macro that is only defined when the
/// rule's `-D<macro>=...` is applied. When rules are applied, compilation is
/// clean; otherwise an undeclared-identifier diagnostic surfaces.

import * as fs from "node:fs";
import * as path from "node:path";
import * as proto from "vscode-languageserver-protocol";
import { expect, test } from "../fixtures.ts";

function messageText(d: proto.Diagnostic): string {
    return typeof d.message === "string" ? d.message : d.message.value;
}

test("baseline without rules", async ({ session }) => {
    const { client } = await session("config_rules_no_config");
    const [uri] = await client.openAndWait("main.cpp");
    client.assertHasErrors(uri, "Expected diagnostics without any rules applied");
    const errors = client.errors(uri);
    expect(
        errors.some((d) => messageText(d).includes("FROM_INIT")),
        `Expected a diagnostic referencing FROM_INIT, got: ${JSON.stringify(errors)}`,
    ).toBe(true);
});

test("rules from toml", async ({ session }) => {
    const { client } = await session("config_rules_toml");
    const [uri] = await client.openAndWait("main.cpp");
    client.assertCleanCompile(uri);

    const symbols = await client.documentSymbols(uri);
    expect(symbols && symbols.length > 0, "Expected document symbols for value()/main()").toBe(
        true,
    );
    const hover = await client.hoverAt(uri, 4, 4); // on 'main'
    expect(hover).not.toBeNull();
});

test("rules from init options", async ({ session }) => {
    const { client } = await session("config_rules_no_config", {
        initializationOptions: { rules: [{ patterns: ["**/*.cpp"], append: ["-DFROM_INIT=1"] }] },
    });
    const [uri] = await client.openAndWait("main.cpp");
    client.assertCleanCompile(uri);
});

test("init options replaces toml rules", async ({ session }) => {
    const { client } = await session("config_rules_toml", {
        initializationOptions: { rules: [{ patterns: ["**/*.cpp"], append: ["-DUNRELATED"] }] },
    });
    const [uri] = await client.openAndWait("main.cpp");
    client.assertHasErrors(uri, "initializationOptions should have overridden clice.toml rules");
    const errors = client.errors(uri);
    expect(
        errors.some((d) => messageText(d).includes("FROM_TOML")),
        `Expected FROM_TOML diagnostic after override, got: ${JSON.stringify(errors)}`,
    ).toBe(true);
});

test("rules pattern mismatch", async ({ session }) => {
    const { client } = await session("config_rules_no_config", {
        initializationOptions: {
            rules: [{ patterns: ["**/does_not_match.cpp"], append: ["-DFROM_INIT=1"] }],
        },
    });
    const [uri] = await client.openAndWait("main.cpp");
    client.assertHasErrors(uri, "Rule pattern should not have matched main.cpp");
});

test("config type error diagnostic", async ({ session }) => {
    // Wrong value type → Error diagnostic on the clice.toml URI; the config
    // falls back to defaults. (Line/column pinpointing awaits the kotatsu
    // TOML error-location feature — see config_tests.cpp.)
    const workspace = session.tmpdir();
    workspace.write("clice.toml", '[project]\ntest_hooks = "yes"\n');
    workspace.write("main.cpp", "int main() { return 0; }\n");
    const client = session.spawn(workspace, {});
    await client.initialize(workspace);
    const tomlUri = workspace.uri("clice.toml");
    await client.waitDiagnostics(tomlUri, 10_000);
    const diags = client.diagnostics.get(tomlUri) ?? [];
    expect(diags.length, `expected one config diagnostic: ${JSON.stringify(diags)}`).toBe(1);
    expect(diags[0]!.severity).toBe(proto.DiagnosticSeverity.Error);
    expect(diags[0]!.message).toContain("test_hooks");
});

test("config unknown key diagnostic", async ({ session }) => {
    // Typo'd key → Warning diagnostic; the rest of the config still applies.
    const workspace = session.tmpdir();
    workspace.write("clice.toml", "[project]\nclang_tdy = true\n");
    workspace.write("main.cpp", "int main() { return 0; }\n");
    const client = session.spawn(workspace, {});
    await client.initialize(workspace);
    const tomlUri = workspace.uri("clice.toml");
    await client.waitDiagnostics(tomlUri, 10_000);
    const diags = client.diagnostics.get(tomlUri) ?? [];
    expect(diags.length, `expected one config diagnostic: ${JSON.stringify(diags)}`).toBe(1);
    expect(diags[0]!.severity).toBe(proto.DiagnosticSeverity.Warning);
    expect(diags[0]!.message).toContain("clang_tdy");
});

test("config diagnostic clears after fix", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write("clice.toml", '[project]\ntest_hooks = "yes"\n');
    workspace.write("main.cpp", "int main() { return 0; }\n");
    let client = session.spawn(workspace, {});
    await client.initialize(workspace);
    const tomlUri = workspace.uri("clice.toml");
    await client.waitDiagnostics(tomlUri, 10_000);
    expect(
        (client.diagnostics.get(tomlUri) ?? []).length,
        "broken config should be diagnosed",
    ).toBeGreaterThan(0);
    await client.shutdown();

    // Fix the config and restart — the new session publishes an empty list
    // for the config URI so stale markers clear.
    workspace.write("clice.toml", "[project]\ntest_hooks = true\n");
    client = session.spawn(workspace, {});
    await client.initialize(workspace);
    await client.waitDiagnostics(tomlUri, 10_000);
    expect(client.diagnostics.get(tomlUri), "fixed config must clear diagnostics").toEqual([]);
});

test("config dump logged", async ({ session }) => {
    // The startup log is the discoverable record of the resolved paths.
    const workspace = session.tmpdir();
    const client = session.spawn(workspace, {});
    await client.initialize(workspace);
    // Shut down before reading so the startup log is fully flushed to disk.
    await client.shutdown();

    const logsDir = workspace.path(".clice/logs");
    const names = fs
        .readdirSync(logsDir, { recursive: true, encoding: "utf8" })
        .filter((name) => path.basename(name) === "master.log");
    expect(names.length, "expected a master.log").toBeGreaterThan(0);
    const text = names.map((name) => fs.readFileSync(path.join(logsDir, name), "utf8")).join("");
    expect(text).toContain("Session log directory:");
    // All three config layers are dumped: file, overlay, merged result.
    expect(text).toContain("Configuration file");
    expect(text).toContain("initializationOptions");
    expect(text).toContain("Effective configuration:");
    expect(text).toContain('"cache_dir"');
});

test("inlay hint options from config", async ({ session }) => {
    // block_end hints are off by default and reachable only through the
    // [inlay_hints] config section, so their appearance proves the options
    // travel master → worker instead of the old hardcoded defaults.
    const source = [
        "int compute() {",
        "    int total = 0;",
        "    for (int i = 0; i < 10; i += 1) {",
        "        total += i;",
        "    }",
        "    return total;",
        "}",
        "",
    ].join("\n");
    const wholeFile = {
        start: { line: 0, character: 0 },
        end: { line: 7, character: 0 },
    };

    const workspace = session.tmpdir();
    workspace.write("main.cpp", source);
    workspace.writeCDB(["main.cpp"]);
    const client = session.spawn(workspace, {});
    await client.initialize(workspace, {
        initializationOptions: { inlay_hints: { block_end: true, deduced_types: false } },
    });
    const [uri] = await client.openAndWait("main.cpp");
    const hints = (await client.inlayHints(uri, wholeFile)) ?? [];
    const labels = hints.map((h) => (typeof h.label === "string" ? h.label : ""));
    expect(labels, `expected a block-end hint, got: ${JSON.stringify(labels)}`).toContain(
        "// compute",
    );
    await client.shutdown();

    // Control: a default-config session must not produce block-end hints.
    const control = session.spawn(workspace, {});
    await control.initialize(workspace);
    const [controlUri] = await control.openAndWait("main.cpp");
    const controlHints = (await control.inlayHints(controlUri, wholeFile)) ?? [];
    const controlLabels = controlHints.map((h) => (typeof h.label === "string" ? h.label : ""));
    expect(controlLabels).not.toContain("// compute");
});
