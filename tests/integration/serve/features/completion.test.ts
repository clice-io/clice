/// Integration tests for #include completion and import completion in clice.

import type { Serve } from "@clice/tools/actions";
import * as proto from "vscode-languageserver-protocol";
import { at, expect, serve, type Loc } from "../../fixtures.ts";

type CompletionReply = proto.CompletionItem[] | proto.CompletionList | null;

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

/// `file` opened and compiled, then its buffer replaced by `text`; returns
/// the position the `|` in `text` marks.
async function typed(s: Serve, file: string, text: string): Promise<Loc> {
    await s.compiled(file);
    s.edit(file, { text: text.replace("|", "") });
    return at(file, text);
}

/// The range from one position to another.
function span(s: Serve, from: Loc, to: Loc): proto.Range {
    return { start: s.position(from).position, end: s.position(to).position };
}

const headers = serve("shapes/headers");
const modules = serve("shapes/modules");
const tiny = serve("tiny");

/// Completion after #include " should list local headers.
headers("include completion quoted", async ({ s }) => {
    await s.compiled(s.file("main"));
    s.edit(s.file("main"), { remove: 'rcle.h"' });

    const result = await s.completion(at(s.file("main"), '#include "shapes/ci|'));

    expect(result).not.toBeNull();
    expect(labelsOf(result)).toContain("circle.h");

    s.close(s.file("main"));
});

/// A header candidate closes the directive, replacing a delimiter already there.
headers("include completion closes directive", async ({ s }) => {
    const header = async (loc: Loc, label: string) =>
        editOf(itemsOf(await s.completion(loc)).find((i) => i.label === label));

    await s.compiled(s.file("main"));
    s.edit(s.file("main"), { remove: 'rcle.h"' });
    expect(await header(at(s.file("main"), '#include "shapes/ci|'), "circle.h")).toEqual({
        range: span(
            s,
            at(s.file("main"), '#include "shapes/|ci'),
            at(s.file("main"), '#include "shapes/ci|'),
        ),
        newText: 'circle.h"',
    });

    s.edit(s.file("main"), { after: '#include "shapes/ci', insert: 'rcle.h"' });
    expect(await header(at(s.file("main"), '#include "shapes/ci|rcle.h"'), "circle.h")).toEqual({
        range: span(
            s,
            at(s.file("main"), '#include "shapes/|circle.h"'),
            at(s.file("main"), '#include "shapes/circle.h"|'),
        ),
        newText: 'circle.h"',
    });

    // Picked in an earlier path component, a header ends the path there.
    await s.compiled(s.file("circle_impl"));
    expect(
        await header(at(s.file("circle_impl"), '#include "shapes/d|etail/math.h"'), "draft.h"),
    ).toEqual({
        range: span(
            s,
            at(s.file("circle_impl"), '#include "shapes/|detail'),
            at(s.file("circle_impl"), 'math.h"|'),
        ),
        newText: 'draft.h"',
    });
});

/// Sources and other non-header files on the search path are not candidates.
headers("include completion lists headers", async ({ s }) => {
    await s.compiled(s.file("registry"));
    s.edit(s.file("registry"), { remove: 'registry.h"' });
    const nested = labelsOf(await s.completion(at(s.file("registry"), '#include "shapes/|')));
    expect(nested).toContain("registry.h");
    expect(nested).toContain("detail/");

    const labels = labelsOf(
        await s.completion(at(s.file("registry"), '#include "|registry_limits.h"')),
    );
    expect(labels).toContain("registry_limits.h");
    expect(labels).toContain("shapes/");
    expect(labels).not.toContain("registry.cpp");
    expect(labels).not.toContain("c_api.c");
});

/// A quoted include finds headers next to the file without any -I.
headers("include completion sibling headers", async ({ s }) => {
    await s.compiled(s.file("registry"));
    s.edit(s.file("registry"), { remove: 'istry_limits.h"' });
    const result = await s.completion(at(s.file("registry"), '#include "reg|'));
    expect(labelsOf(result)).toContain("registry_limits.h");
});

/// An identifier named `import` opening a line is not an import statement.
tiny("import identifier member access", async ({ s }) => {
    const loc = await typed(
        s,
        "main.cpp",
        "struct S { int member; };\nvoid f(S* import) {\nimport->|\n}",
    );

    const result = await s.completion(loc, ">");
    expect(labelsOf(result)).toContain("member");
});

/// Completion for #include "subdir/ should list files in subdir.
headers("include completion subdirectory", async ({ s }) => {
    await s.compiled(s.file("circle_impl"));
    s.edit(s.file("circle_impl"), { remove: 'math.h"' });

    const result = await s.completion(at(s.file("circle_impl"), '#include "shapes/detail/|'));

    expect(result).not.toBeNull();
    expect(labelsOf(result)).toContain("math.h");

    s.close(s.file("circle_impl"));
});

/// Completion after #include < should list system headers.
tiny("include completion angle bracket", async ({ s }) => {
    const loc = await typed(s, "main.cpp", "#include <cstd|");

    const result = await s.completion(loc);

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
headers("no include completion on regular code", async ({ s }) => {
    await s.compiled(s.file("main"));

    const result = await s.completion(at(s.file("main"), "double total = |shapes::area"));

    // Should return results from clang (keywords, etc.), not include paths.
    // Verify none of the results look like header filenames.
    expect(result).not.toBeNull();
    const labels = labelsOf(result);
    expect(labels).not.toContain("circle.h");
    expect(labels).not.toContain("math.h");

    s.close(s.file("main"));
});

/// Completion after #include " with no prefix lists the search path's entries.
headers("include completion empty prefix", async ({ s }) => {
    await s.compiled(s.file("registry"));
    s.edit(s.file("registry"), { remove: 'registry_limits.h"' });

    const result = await s.completion(at(s.file("registry"), 'registry.h"\n#include "|'));

    expect(result).not.toBeNull();
    const labels = labelsOf(result);
    expect(labels).toContain("registry_limits.h");
    expect(labels).toContain("shapes/");
    expect(labels).toContain("legacy/");

    s.close(s.file("registry"));
});

/// Import completion should list known modules.
modules("import completion basic", async ({ s }) => {
    await s.compiled(s.file("main"));
    s.edit(s.file("main"), { remove: "shapes;" });

    const result = await s.completion(at(s.file("main"), "import |"));

    expect(result).not.toBeNull();
    const labels = labelsOf(result);
    expect(labels, `Expected 'shapes' in completion labels, got: ${labels.join(", ")}`).toContain(
        "shapes",
    );
    const cursor = s.position(at(s.file("main"), "import |")).position;
    expect(editOf(itemsOf(result).find((i) => i.label === "shapes"))).toEqual({
        range: { start: cursor, end: cursor },
        newText: "shapes;",
    });
});

/// Space-triggered completion on an import line lists modules.
modules("space trigger serves import", async ({ s }) => {
    await s.compiled(s.file("main"));
    s.edit(s.file("main"), { remove: "shapes;" });

    const result = await s.completion(at(s.file("main"), "import |"), " ");

    expect(result).not.toBeNull();
    const labels = labelsOf(result);
    expect(labels, `Expected 'shapes' in completion labels, got: ${labels.join(", ")}`).toContain(
        "shapes",
    );
});

/// A `:` typed after `import` lists the current module's partitions; an
/// interface unit is offered its interface partitions only.
modules("colon trigger serves partitions", async ({ s }) => {
    await s.compiled(s.file("circle_impl"));
    s.edit(s.file("circle_impl"), { remove: "detail;" });
    const fromImpl = await s.completion(at(s.file("circle_impl"), "import :|"), ":");
    expect(labelsOf(fromImpl)).toEqual([":circle", ":detail", ":polygon", ":shape"]);

    await s.compiled(s.file("circle"));
    s.edit(s.file("circle"), { remove: "shape;" });
    const fromInterface = await s.completion(at(s.file("circle"), "import :|"), ":");
    expect(labelsOf(fromInterface)).toEqual([":polygon", ":shape"]);
});

/// Space-triggered completion outside import lines returns no items.
tiny("space trigger gated elsewhere", async ({ s }) => {
    s.open("main.cpp");

    // Cursor right after "return " — a space trigger here must be answered
    // with an empty list instead of a full completion build.
    const result = await s.completion(at("main.cpp", "return |lhs"), " ");

    const items = labelsOf(result);
    expect(
        items.length,
        `Expected no items for gated space trigger, got: ${items.join(", ")}`,
    ).toBe(0);
});

/// Space-triggered completion in an include context returns no items.
tiny("space trigger gated include", async ({ s }) => {
    const loc = await typed(s, "main.cpp", "#include <vector> |");

    // The space gate must run before include scanning: no directory
    // enumeration and no candidates for a trailing-space trigger.
    const result = await s.completion(loc, " ");

    const items = labelsOf(result);
    expect(
        items.length,
        `Expected no items for gated space trigger, got: ${items.join(", ")}`,
    ).toBe(0);
});

/// `<` opening a template parameter list is not a completion point.
tiny("angle trigger gated template", async ({ s }) => {
    const loc = await typed(s, "main.cpp", "#define UNRELATED_MACRO 123\ntemplate<|");

    const result = await s.completion(loc, "<");

    const items = labelsOf(result);
    expect(items.length, `Expected no items after template<, got: ${items.join(", ")}`).toBe(0);
});

/// The third dot of a parameter pack is not a member access.
tiny("ellipsis trigger gated", async ({ s }) => {
    const loc = await typed(s, "main.cpp", "template<typename...|");

    const result = await s.completion(loc, ".");

    const items = labelsOf(result);
    expect(items.length, `Expected no items after an ellipsis, got: ${items.join(", ")}`).toBe(0);
});

/// A dot after an object still completes its members on the trigger path.
tiny("dot trigger serves member", async ({ s }) => {
    const loc = await typed(
        s,
        "main.cpp",
        "struct Widget { int member; };\nvoid f() { Widget w; w.| }",
    );

    const result = await s.completion(loc, ".");

    expect(labelsOf(result)).toContain("member");
});

/// Import completion with prefix should filter to matching modules.
modules("import completion with prefix", async ({ s }) => {
    await s.compiled(s.file("main"));
    s.edit(s.file("main"), { remove: "pes;" });

    const result = await s.completion(at(s.file("main"), "import sha|"));

    expect(result).not.toBeNull();
    const labels = labelsOf(result);
    expect(labels, `Expected 'shapes' in completion labels, got: ${labels.join(", ")}`).toContain(
        "shapes",
    );
});

/// Import completion should return dotted module names like my.io.
serve("modules/dotted_module_name")("import completion dotted names", async ({ s }) => {
    // Open both module files to register them.
    await s.compiled("io.cppm");
    await s.compiled("app.cppm");

    s.edit("app.cppm", { remove: "io;" });

    const result = await s.completion(at("app.cppm", "import my.|"));

    expect(result).not.toBeNull();
    const labels = labelsOf(result);
    expect(labels, `Expected dotted module names, got: ${labels.join(", ")}`).toContain("my.io");
});

/// Adding import in buffer (unsaved) should still build the needed PCM.
modules("buffer aware module deps", async ({ s }) => {
    // Open the module file first so it gets scanned.
    await s.compiled(s.file("detail"));

    // The disk text of measure.cpp needs no :detail; the buffer does.
    s.open(s.file("measure"));
    s.edit(
        s.file("measure"),
        { after: "module shapes;\n", insert: "\nimport :detail;\n" },
        { replace: "return pi;", with: "return detail::square(pi);" },
    );

    // Should have no errors if the partition's PCM was built from the buffer scan.
    const errors = await s.errors(s.file("measure"));
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
serve("tiny", { config: SNIPPET_OPTIONS })("snippets follow client support", async ({ s }) => {
    s.disk.edit("main.cpp", { replace: "add(1, 2)", with: "ad" });
    const loc = at("main.cpp", "int value = ad|");
    await s.compiled("main.cpp");
    const plain = itemsOf(await s.completion(loc)).find((item) => item.label === "add");
    expect(plain?.insertTextFormat).not.toBe(proto.InsertTextFormat.Snippet);
    expect(plain?.textEdit?.newText).toBe("add()");
    await s.stop();

    // The case's server declares no snippet support; this one does.
    await s.start({
        capabilities: {
            textDocument: { completion: { completionItem: { snippetSupport: true } } },
        },
    });
    await s.compiled("main.cpp");
    const placeholders = itemsOf(await s.completion(loc)).find((item) => item.label === "add");
    expect(placeholders?.insertTextFormat).toBe(proto.InsertTextFormat.Snippet);
    expect(placeholders?.textEdit?.newText).toBe("add(${1:int lhs}, ${2:int rhs})");
});
