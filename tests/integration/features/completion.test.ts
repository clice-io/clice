/// Integration tests for #include completion and import completion in clice.

import * as proto from "vscode-languageserver-protocol";
import { withTimeout } from "@clice/tools/client";
import { test, expect } from "../../fixtures.ts";

function labelsOf(result: proto.CompletionItem[] | proto.CompletionList | null): string[] {
    const items = Array.isArray(result) ? result : (result?.items ?? []);
    return items.map((item) => item.label);
}

/// Completion after #include " should list local headers.
test("include completion quoted", async ({ session }) => {
    const { client } = await session("include_completion");
    const [uri] = await client.openAndWait("main.cpp");

    // Update content to trigger include completion for "my" prefix.
    client.change(uri, 1, '#include "my');

    const result = await client.completionAt(uri, 0, 12); // After "my"

    expect(result).not.toBeNull();
    expect(labelsOf(result)).toContain("myheader.h");

    client.close(uri);
});

/// Completion for #include "subdir/ should list files in subdir.
test("include completion subdirectory", async ({ session }) => {
    const { client } = await session("include_completion");
    const [uri] = await client.openAndWait("main.cpp");

    client.change(uri, 1, '#include "subdir/');

    const result = await client.completionAt(uri, 0, 17); // After "subdir/"

    expect(result).not.toBeNull();
    expect(labelsOf(result)).toContain("nested.h");

    client.close(uri);
});

/// Completion after #include < should list system headers.
test("include completion angle bracket", async ({ session }) => {
    const { client } = await session("include_completion");
    const [uri] = await client.openAndWait("main.cpp");

    client.change(uri, 1, "#include <cstd");

    const result = await client.completionAt(uri, 0, 14); // After "cstd"

    expect(result).not.toBeNull();
    const labels = labelsOf(result);
    // Should contain at least some standard library headers starting with "cstd".
    const cstdLabels = labels.filter((name) => name.startsWith("cstd"));
    expect(cstdLabels.length, `Expected cstd* headers, got: ${labels.join(", ")}`).toBeGreaterThan(
        0,
    );

    client.close(uri);
});

/// Regular code should NOT trigger include completion (goes to worker).
test("no include completion on regular code", async ({ session }) => {
    const { client } = await session("include_completion");
    const [uri] = await client.openAndWait("main.cpp");

    client.change(uri, 1, "int x = ");

    const result = await client.completionAt(uri, 0, 8);

    // Should return results from clang (keywords, etc.), not include paths.
    // Verify none of the results look like header filenames.
    expect(result).not.toBeNull();
    const labels = labelsOf(result);
    expect(labels).not.toContain("myheader.h");
    expect(labels).not.toContain("nested.h");

    client.close(uri);
});

/// Completion after #include " with no prefix should list all local headers.
test("include completion empty prefix", async ({ session }) => {
    const { client } = await session("include_completion");
    const [uri] = await client.openAndWait("main.cpp");

    client.change(uri, 1, '#include "');

    const result = await client.completionAt(uri, 0, 10); // Right after the quote

    expect(result).not.toBeNull();
    // With empty prefix, should list available headers including myheader.h
    // and the subdir/ directory entry.
    expect(labelsOf(result)).toContain("myheader.h");

    client.close(uri);
});

/// Import completion should list known modules.
test("import completion basic", async ({ session }) => {
    const { client } = await session("modules/chained_modules");
    // First open mod_a to ensure it's scanned and module A is registered.
    await client.openAndWait("mod_a.cppm");

    // Open mod_b and change its content to an incomplete import line.
    const [uriB] = client.open("mod_b.cppm");
    client.change(uriB, 1, "import ");

    const result = await client.completionAt(uriB, 0, 7);

    expect(result).not.toBeNull();
    const labels = labelsOf(result);
    expect(labels, `Expected 'A' in completion labels, got: ${labels.join(", ")}`).toContain("A");
});

/// Space-triggered completion on an import line lists modules.
test("space trigger serves import", async ({ session }) => {
    const { client } = await session("modules/chained_modules");
    await client.openAndWait("mod_a.cppm");

    const [uriB] = client.open("mod_b.cppm");
    client.change(uriB, 1, "import ");

    const result = await client.completionAt(uriB, 0, 7, { triggerCharacter: " " });

    expect(result).not.toBeNull();
    const labels = labelsOf(result);
    expect(labels, `Expected 'A' in completion labels, got: ${labels.join(", ")}`).toContain("A");
});

/// Space-triggered completion outside import lines returns no items.
test("space trigger gated elsewhere", async ({ session }) => {
    const { client } = await session("modules/chained_modules");
    const [uriB] = client.open("mod_b.cppm");
    client.change(uriB, 1, "int main() { return 0; }");

    // Cursor right after "return " — a space trigger here must be answered
    // with an empty list instead of a full completion build.
    const result = await client.completionAt(uriB, 0, 20, { triggerCharacter: " " });

    const items = labelsOf(result);
    expect(
        items.length,
        `Expected no items for gated space trigger, got: ${items.join(", ")}`,
    ).toBe(0);
});

/// Space-triggered completion in an include context returns no items.
test("space trigger gated include", async ({ session }) => {
    const { client } = await session("include_completion");
    const [uri] = await client.openAndWait("main.cpp");
    client.change(uri, 1, "#include <vector> ");

    // The space gate must run before include scanning: no directory
    // enumeration and no candidates for a trailing-space trigger.
    const result = await client.completionAt(uri, 0, 18, { triggerCharacter: " " });

    const items = labelsOf(result);
    expect(
        items.length,
        `Expected no items for gated space trigger, got: ${items.join(", ")}`,
    ).toBe(0);
});

/// Import completion with prefix should filter to matching modules.
test("import completion with prefix", async ({ session }) => {
    const { client } = await session("modules/chained_modules");
    // Open mod_a to register module A.
    await client.openAndWait("mod_a.cppm");

    // Open mod_b and type 'import A' (with prefix).
    const [uriB] = client.open("mod_b.cppm");
    client.change(uriB, 1, "import A");

    const result = await client.completionAt(uriB, 0, 8);

    expect(result).not.toBeNull();
    const labels = labelsOf(result);
    expect(labels, `Expected 'A' in completion labels, got: ${labels.join(", ")}`).toContain("A");
});

/// Import completion should return dotted module names like my.app and my.io.
test("import completion dotted names", async ({ session }) => {
    const { client } = await session("modules/dotted_module_name");
    // Open both module files to register them.
    await client.openAndWait("io.cppm");
    await client.openAndWait("app.cppm");

    // Change app.cppm to an incomplete import with dotted prefix.
    const [uriApp] = client.open("app.cppm");
    client.change(uriApp, 1, "import my.");

    const result = await client.completionAt(uriApp, 0, 10);

    expect(result).not.toBeNull();
    const labels = labelsOf(result);
    expect(
        labels.includes("my.app") || labels.includes("my.io"),
        `Expected dotted module names in completion labels, got: ${labels.join(", ")}`,
    ).toBe(true);
});

/// Adding import in buffer (unsaved) should still build the needed PCM.
test("buffer aware module deps", async ({ session }) => {
    const { client } = await session("modules/consumer_imports_module");
    // Open the module file first so it gets scanned.
    await client.openAndWait("math.cppm");

    // Open main.cpp with new content that imports Math (simulating unsaved edit).
    const [uri] = client.open("main.cpp");
    client.change(uri, 1, "import Math;\nint x = add(1, 2);\n");

    // Trigger compilation via hover (pull-based model).
    const arrived = client.armDiagnostics(uri);
    await client.hoverAt(uri, 0, 0);

    // Wait for diagnostics.
    await withTimeout(arrived, 60_000, `diagnostics ${uri}`);

    // Should have no errors if Math PCM was built successfully from buffer scan.
    const errors = client.errors(uri);
    expect(errors.length, `Expected no errors, got: ${JSON.stringify(errors)}`).toBe(0);
});
