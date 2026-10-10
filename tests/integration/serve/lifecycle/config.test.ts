/// Integration tests for clice configuration (clice.toml + initializationOptions).
///
/// The rules cases write a main.cpp that references a macro only defined
/// when the rule's `-D<macro>=...` is applied. When rules are applied,
/// compilation is clean; otherwise an undeclared-identifier diagnostic
/// surfaces.

import * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import { at, expect, serve } from "../../fixtures.ts";

function messageText(d: proto.Diagnostic): string {
    return typeof d.message === "string" ? d.message : d.message.value;
}

/// A main.cpp that compiles only when a rule defines `macro`.
function needing(macro: string): Record<string, string> {
    return {
        "main.cpp": `int value() {\n    return ${macro};\n}\n\nint main() {\n    return value();\n}\n`,
    };
}

const TOML_RULES = '[[rules]]\npatterns = ["**/*.cpp"]\nappend = ["-DFROM_TOML"]\n';

serve("tiny", { files: needing("FROM_INIT") })("baseline without rules", async ({ s }) => {
    const errors = await s.errors("main.cpp");
    expect(errors.length, "Expected diagnostics without any rules applied").toBeGreaterThan(0);
    expect(
        errors.some((d) => messageText(d).includes("FROM_INIT")),
        `Expected a diagnostic referencing FROM_INIT, got: ${JSON.stringify(errors)}`,
    ).toBe(true);
});

serve("tiny", { files: { ...needing("FROM_TOML"), "clice.toml": TOML_RULES } })(
    "rules from toml",
    async ({ s }) => {
        await s.clean("main.cpp");
        const symbols = await s.request<proto.DocumentSymbol[] | null>(
            "textDocument/documentSymbol",
            "main.cpp",
        );
        expect(symbols && symbols.length > 0, "Expected document symbols for value()/main()").toBe(
            true,
        );
        expect(await s.hover(at("main.cpp", "int |main"))).not.toBeNull();
    },
);

serve("tiny", {
    files: needing("FROM_INIT"),
    config: { rules: [{ patterns: ["**/*.cpp"], append: ["-DFROM_INIT=1"] }] },
})("rules from init options", async ({ s }) => {
    await s.clean("main.cpp");
});

serve("tiny", {
    files: { ...needing("FROM_TOML"), "clice.toml": TOML_RULES },
    config: { rules: [{ patterns: ["**/*.cpp"], append: ["-DUNRELATED"] }] },
})("init options replaces toml rules", async ({ s }) => {
    const errors = await s.errors("main.cpp");
    expect(
        errors.length,
        "initializationOptions should have overridden clice.toml rules",
    ).toBeGreaterThan(0);
    expect(
        errors.some((d) => messageText(d).includes("FROM_TOML")),
        `Expected FROM_TOML diagnostic after override, got: ${JSON.stringify(errors)}`,
    ).toBe(true);
});

serve("tiny", {
    files: needing("FROM_INIT"),
    config: { rules: [{ patterns: ["**/does_not_match.cpp"], append: ["-DFROM_INIT=1"] }] },
})("rules pattern mismatch", async ({ s }) => {
    expect(
        (await s.errors("main.cpp")).length,
        "Rule pattern should not have matched main.cpp",
    ).toBeGreaterThan(0);
});

serve("tiny", { files: { "clice.toml": '[project]\ntest_hooks = "yes"\n' } })(
    "config type error diagnostic",
    async ({ s }) => {
        // Wrong value type → Error diagnostic on the clice.toml URI; the config
        // falls back to defaults. (Line/column pinpointing awaits the kotatsu
        // TOML error-location feature — see config_tests.cpp.)
        const diags = (await s.pushed("clice.toml")) ?? [];
        expect(diags.length, `expected one config diagnostic: ${JSON.stringify(diags)}`).toBe(1);
        expect(diags[0]!.severity).toBe(proto.DiagnosticSeverity.Error);
        expect(diags[0]!.message).toContain("test_hooks");
    },
);

serve("tiny", { files: { "clice.toml": "[project]\nclang_tdy = true\n" } })(
    "config unknown key diagnostic",
    async ({ s }) => {
        // Typo'd key → Warning diagnostic; the rest of the config still applies.
        const diags = (await s.pushed("clice.toml")) ?? [];
        expect(diags.length, `expected one config diagnostic: ${JSON.stringify(diags)}`).toBe(1);
        expect(diags[0]!.severity).toBe(proto.DiagnosticSeverity.Warning);
        expect(diags[0]!.message).toContain("clang_tdy");
    },
);

serve("tiny", { files: { "clice.toml": '[project]\ntest_hooks = "yes"\n' } })(
    "config diagnostic clears after fix",
    async ({ s }) => {
        expect(
            (await s.pushed("clice.toml"))?.length ?? 0,
            "broken config should be diagnosed",
        ).toBeGreaterThan(0);
        // Fix the config and restart — the new session publishes an empty list
        // for the config URI so stale markers clear.
        await s.offline(() => {
            s.disk.write("clice.toml", "[project]\ntest_hooks = true\n");
        });
        expect(await s.pushed("clice.toml"), "fixed config must clear diagnostics").toEqual([]);
    },
);

serve("tiny")("config dump logged", async ({ s }) => {
    // The startup log is the discoverable record of the resolved paths.
    // Shut down before reading so the startup log is fully flushed to disk.
    await s.stop();
    const text = s.workspace.log("master.log");
    expect(text).toContain("Session log directory:");
    // All three config layers are dumped: file, overlay, merged result.
    expect(text).toContain("Configuration file");
    expect(text).toContain("initializationOptions");
    expect(text).toContain("Effective configuration:");
    expect(text).toContain('"cache_dir"');
});

// block_end hints are off by default and reachable only through the
// [inlay_hints] config section, so their appearance proves the options
// travel master → worker instead of the old hardcoded defaults.
const INLAY_SOURCE = [
    "int compute() {",
    "    int total = 0;",
    "    for (int i = 0; i < 10; i += 1) {",
    "        total += i;",
    "    }",
    "    return total;",
    "}",
    "",
].join("\n");

async function inlayLabels(s: Serve): Promise<string[]> {
    await s.compiled("main.cpp");
    const range = { start: { line: 0, character: 0 }, end: { line: 7, character: 0 } };
    const hints =
        (await s.request<proto.InlayHint[] | null>("textDocument/inlayHint", "main.cpp", {
            range,
        })) ?? [];
    return hints.map((h) => (typeof h.label === "string" ? h.label : ""));
}

serve.files(
    { "main.cpp": INLAY_SOURCE },
    { config: { inlay_hints: { block_end: true, deduced_types: false } } },
)("inlay hint options from config", async ({ s }) => {
    const labels = await inlayLabels(s);
    expect(labels, `expected a block-end hint, got: ${JSON.stringify(labels)}`).toContain(
        "// compute",
    );
});

// Control: a default-config session must not produce block-end hints.
serve.files({ "main.cpp": INLAY_SOURCE })("inlay hint options default", async ({ s }) => {
    expect(await inlayLabels(s)).not.toContain("// compute");
});
