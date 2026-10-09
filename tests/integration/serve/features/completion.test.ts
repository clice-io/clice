/// Integration tests for #include completion and import completion in clice.

import type { Serve } from "@clice/tools/actions";
import { positionAt } from "@clice/tools/client/edits";
import * as proto from "vscode-languageserver-protocol";
import { at, expect, serve, type Loc } from "../../fixtures.ts";

type CompletionReply = proto.CompletionItem[] | proto.CompletionList | null;

function complete(s: Serve, loc: Loc, triggerCharacter?: string): Promise<CompletionReply> {
    const context =
        triggerCharacter === undefined
            ? {}
            : {
                  context: {
                      triggerKind: proto.CompletionTriggerKind.TriggerCharacter,
                      triggerCharacter,
                  },
              };
    return s.request("textDocument/completion", loc, context) as Promise<CompletionReply>;
}

function itemsOf(result: CompletionReply): proto.CompletionItem[] {
    return Array.isArray(result) ? result : (result?.items ?? []);
}

function labelsOf(result: CompletionReply): string[] {
    return itemsOf(result).map((item) => item.label);
}

function editOf(item: proto.CompletionItem | undefined): proto.TextEdit | undefined {
    const edit = item?.textEdit;
    return edit !== undefined && "range" in edit ? edit : undefined;
}

/// `file` opened and compiled, then its buffer replaced by `text`.
async function typed(s: Serve, file: string, text: string): Promise<void> {
    await s.compiled(file);
    s.edit(file, { text });
}

const includes = serve.data("include_completion");
const chained = serve.data("modules/chained_modules");

/// Completion after #include " should list local headers.
includes("include completion quoted", async ({ s }) => {
    await typed(s, "main.cpp", '#include "my');

    const result = await complete(s, at("main.cpp", '#include "my|'));

    expect(result).not.toBeNull();
    expect(labelsOf(result)).toContain("myheader.h");

    s.close("main.cpp");
});

/// A header candidate closes the directive, replacing a delimiter already there.
includes("include completion closes directive", async ({ s }) => {
    await typed(s, "main.cpp", '#include "my');
    let item = itemsOf(await complete(s, at("main.cpp", '#include "my|'))).find(
        (i) => i.label === "myheader.h",
    );
    expect(editOf(item)).toEqual({
        range: { start: { line: 0, character: 10 }, end: { line: 0, character: 12 } },
        newText: 'myheader.h"',
    });

    s.edit("main.cpp", { text: '#include "myhe"' });
    item = itemsOf(await complete(s, at("main.cpp", '#include "my|he"'))).find(
        (i) => i.label === "myheader.h",
    );
    expect(editOf(item)).toEqual({
        range: { start: { line: 0, character: 10 }, end: { line: 0, character: 15 } },
        newText: 'myheader.h"',
    });

    // Picked in an earlier path component, a header ends the path there.
    s.edit("main.cpp", { text: '#include "my/rest.h"' });
    item = itemsOf(await complete(s, at("main.cpp", '#include "my|/rest.h"'))).find(
        (i) => i.label === "myheader.h",
    );
    expect(editOf(item)).toEqual({
        range: { start: { line: 0, character: 10 }, end: { line: 0, character: 20 } },
        newText: 'myheader.h"',
    });
});

/// Sources and other non-header files on the search path are not candidates.
includes("include completion lists headers", async ({ s }) => {
    await typed(s, "main.cpp", '#include "');
    const labels = labelsOf(await complete(s, at("main.cpp", '#include "|')));
    expect(labels).toContain("myheader.h");
    expect(labels).toContain("subdir/");
    expect(labels).not.toContain("main.cpp");
    expect(labels).not.toContain("compile_commands.json");
});

/// A quoted include finds headers next to the file without any -I.
serve.files(
    { "src/main.cpp": "int main() {}\n", "src/local.h": "#pragma once\n" },
    { manifest: { cxx: ["-std=c++17"], units: { "src/main.cpp": [] } } },
)("include completion sibling headers", async ({ s }) => {
    await typed(s, "src/main.cpp", '#include "lo');
    expect(labelsOf(await complete(s, at("src/main.cpp", '#include "lo|')))).toContain("local.h");
});

/// An identifier named `import` opening a line is not an import statement.
includes("import identifier member access", async ({ s }) => {
    await typed(s, "main.cpp", "struct S { int member; };\nvoid f(S* import) {\nimport->\n}");

    const result = await complete(s, at("main.cpp", "import->|"), ">");
    expect(labelsOf(result)).toContain("member");
});

/// Completion for #include "subdir/ should list files in subdir.
includes("include completion subdirectory", async ({ s }) => {
    await typed(s, "main.cpp", '#include "subdir/');

    const result = await complete(s, at("main.cpp", '#include "subdir/|'));

    expect(result).not.toBeNull();
    expect(labelsOf(result)).toContain("nested.h");

    s.close("main.cpp");
});

/// Completion after #include < should list system headers.
includes("include completion angle bracket", async ({ s }) => {
    await typed(s, "main.cpp", "#include <cstd");

    const result = await complete(s, at("main.cpp", "#include <cstd|"));

    expect(result).not.toBeNull();
    const labels = labelsOf(result);
    // Should contain at least some standard library headers starting with "cstd".
    const cstdLabels = labels.filter((name) => name.startsWith("cstd"));
    expect(cstdLabels.length, `Expected cstd* headers, got: ${labels.join(", ")}`).toBeGreaterThan(
        0,
    );

    s.close("main.cpp");
});

/// Regular code should NOT trigger include completion (goes to worker).
includes("no include completion on regular code", async ({ s }) => {
    await typed(s, "main.cpp", "int x = ");

    const result = await complete(s, at("main.cpp", "int x = |"));

    // Should return results from clang (keywords, etc.), not include paths.
    // Verify none of the results look like header filenames.
    expect(result).not.toBeNull();
    const labels = labelsOf(result);
    expect(labels).not.toContain("myheader.h");
    expect(labels).not.toContain("nested.h");

    s.close("main.cpp");
});

/// Completion after #include " with no prefix should list all local headers.
includes("include completion empty prefix", async ({ s }) => {
    await typed(s, "main.cpp", '#include "');

    const result = await complete(s, at("main.cpp", '#include "|'));

    expect(result).not.toBeNull();
    // With empty prefix, should list available headers including myheader.h
    // and the subdir/ directory entry.
    expect(labelsOf(result)).toContain("myheader.h");

    s.close("main.cpp");
});

/// Import completion should list known modules.
chained("import completion basic", async ({ s }) => {
    // First open mod_a to ensure it's scanned and module A is registered.
    await s.compiled("mod_a.cppm");

    // Open mod_b and change its content to an incomplete import line.
    s.open("mod_b.cppm");
    s.edit("mod_b.cppm", { text: "import " });

    const result = await complete(s, at("mod_b.cppm", "import |"));

    expect(result).not.toBeNull();
    const labels = labelsOf(result);
    expect(labels, `Expected 'A' in completion labels, got: ${labels.join(", ")}`).toContain("A");
    expect(editOf(itemsOf(result).find((i) => i.label === "A"))).toEqual({
        range: { start: { line: 0, character: 7 }, end: { line: 0, character: 7 } },
        newText: "A;",
    });
});

/// Space-triggered completion on an import line lists modules.
chained("space trigger serves import", async ({ s }) => {
    await s.compiled("mod_a.cppm");

    s.open("mod_b.cppm");
    s.edit("mod_b.cppm", { text: "import " });

    const result = await complete(s, at("mod_b.cppm", "import |"), " ");

    expect(result).not.toBeNull();
    const labels = labelsOf(result);
    expect(labels, `Expected 'A' in completion labels, got: ${labels.join(", ")}`).toContain("A");
});

/// A `:` typed after `import` lists the current module's partitions; an
/// interface unit is offered its interface partitions only.
serve.data("modules/internal_partitions")("colon trigger serves partitions", async ({ s }) => {
    await typed(s, "impl.cpp", "module Lib;\nimport :");
    const fromImpl = await complete(s, at("impl.cpp", "import :|"), ":");
    expect(labelsOf(fromImpl)).toEqual([":api", ":detail", ":util"]);

    await typed(s, "lib.cppm", "export module Lib;\nexport import :");
    const fromInterface = await complete(s, at("lib.cppm", "export import :|"), ":");
    expect(labelsOf(fromInterface)).toEqual([":api"]);
});

/// Space-triggered completion outside import lines returns no items.
chained("space trigger gated elsewhere", async ({ s }) => {
    s.open("mod_b.cppm");
    s.edit("mod_b.cppm", { text: "int main() { return 0; }" });

    // Cursor right after "return " — a space trigger here must be answered
    // with an empty list instead of a full completion build.
    const result = await complete(s, at("mod_b.cppm", "return |0"), " ");

    const items = labelsOf(result);
    expect(
        items.length,
        `Expected no items for gated space trigger, got: ${items.join(", ")}`,
    ).toBe(0);
});

/// Space-triggered completion in an include context returns no items.
includes("space trigger gated include", async ({ s }) => {
    await typed(s, "main.cpp", "#include <vector> ");

    // The space gate must run before include scanning: no directory
    // enumeration and no candidates for a trailing-space trigger.
    const result = await complete(s, at("main.cpp", "#include <vector> |"), " ");

    const items = labelsOf(result);
    expect(
        items.length,
        `Expected no items for gated space trigger, got: ${items.join(", ")}`,
    ).toBe(0);
});

/// `<` opening a template parameter list is not a completion point.
includes("angle trigger gated template", async ({ s }) => {
    await typed(s, "main.cpp", "#define UNRELATED_MACRO 123\ntemplate<");

    const result = await complete(s, at("main.cpp", "template<|"), "<");

    const items = labelsOf(result);
    expect(items.length, `Expected no items after template<, got: ${items.join(", ")}`).toBe(0);
});

/// The third dot of a parameter pack is not a member access.
includes("ellipsis trigger gated", async ({ s }) => {
    await typed(s, "main.cpp", "template<typename...");

    const result = await complete(s, at("main.cpp", "template<typename...|"), ".");

    const items = labelsOf(result);
    expect(items.length, `Expected no items after an ellipsis, got: ${items.join(", ")}`).toBe(0);
});

/// A dot after an object still completes its members on the trigger path.
includes("dot trigger serves member", async ({ s }) => {
    await typed(s, "main.cpp", "struct Widget { int member; };\nvoid f() { Widget w; w. }");

    const result = await complete(s, at("main.cpp", "w.| }"), ".");

    expect(labelsOf(result)).toContain("member");
});

/// Import completion with prefix should filter to matching modules.
chained("import completion with prefix", async ({ s }) => {
    // Open mod_a to register module A.
    await s.compiled("mod_a.cppm");

    // Open mod_b and type 'import A' (with prefix).
    s.open("mod_b.cppm");
    s.edit("mod_b.cppm", { text: "import A" });

    const result = await complete(s, at("mod_b.cppm", "import A|"));

    expect(result).not.toBeNull();
    const labels = labelsOf(result);
    expect(labels, `Expected 'A' in completion labels, got: ${labels.join(", ")}`).toContain("A");
});

/// Import completion should return dotted module names like my.app and my.io.
serve.data("modules/dotted_module_name")("import completion dotted names", async ({ s }) => {
    // Open both module files to register them.
    await s.compiled("io.cppm");
    await s.compiled("app.cppm");

    // Change app.cppm to an incomplete import with dotted prefix.
    s.edit("app.cppm", { text: "import my." });

    const result = await complete(s, at("app.cppm", "import my.|"));

    expect(result).not.toBeNull();
    const labels = labelsOf(result);
    expect(
        labels.includes("my.app") || labels.includes("my.io"),
        `Expected dotted module names in completion labels, got: ${labels.join(", ")}`,
    ).toBe(true);
});

/// Adding import in buffer (unsaved) should still build the needed PCM.
serve.data("modules/consumer_imports_module")("buffer aware module deps", async ({ s }) => {
    // Open the module file first so it gets scanned.
    await s.compiled("math.cppm");

    // Open main.cpp with new content that imports Math (simulating unsaved edit).
    s.open("main.cpp");
    s.edit("main.cpp", { text: "import Math;\nint x = add(1, 2);\n" });

    // Should have no errors if Math PCM was built successfully from buffer scan.
    const errors = await s.errors("main.cpp");
    expect(errors.length, `Expected no errors, got: ${JSON.stringify(errors)}`).toBe(0);
});

const SNIPPET_OPTIONS = {
    code_completion: {
        bundle_overloads: false,
        enable_function_arguments_snippet: true,
        insert_paren_in_function_call: true,
    },
};

/// Snippets reach only clients that declare snippet support.
serve.files(
    { "main.cpp": "int compute(int x);\nvoid f() { compu; }\n" },
    { config: SNIPPET_OPTIONS, manifest: { cxx: ["-std=c++17"], units: { "main.cpp": [] } } },
)("snippets follow client support", async ({ s }) => {
    const loc = at("main.cpp", "compu|;");
    await s.compiled("main.cpp");
    const plain = itemsOf(await complete(s, loc)).find((item) => item.label === "compute");
    expect(plain?.insertTextFormat).not.toBe(proto.InsertTextFormat.Snippet);
    expect(plain?.textEdit?.newText).toBe("compute()");
    await s.stop();

    // The case's server declares no snippet support; this client does.
    const snippets = s.session.spawn(s.workspace);
    await snippets.initialize(s.workspace, {
        initializationOptions: SNIPPET_OPTIONS,
        capabilities: {
            textDocument: { completion: { completionItem: { snippetSupport: true } } },
        },
    });
    const [uri] = await snippets.openAndWait("main.cpp");
    const text = s.disk.read("main.cpp");
    const { line, character } = positionAt(text, text.indexOf("compu;") + "compu".length);
    const result = await snippets.completionAt(uri, line, character);
    const placeholders = itemsOf(result).find((item) => item.label === "compute");
    expect(placeholders?.insertTextFormat).toBe(proto.InsertTextFormat.Snippet);
    expect(placeholders?.textEdit?.newText).toBe("compute(${1:int x})");
});
