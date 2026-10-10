/// PCH (precompiled header) builds of a unit's preamble: when one is built,
/// reused and built again.

import { at, expect, serve } from "../../fixtures.ts";

serve("stdlib")("unchanged preamble keeps its pch", async ({ s }) => {
    // The standard library's lookups (`#include_next`, `__has_include`)
    // fail in some directories on the way: none of that is a change.
    await s.clean("report.cpp");
    const built = s.workspace.pchFiles();
    expect(built).toHaveLength(1);
    await s.completion(at("report.cpp", "|std::string report("));
    await s.sync();
    expect((await s.counts()).pch, "the pch was rebuilt").toBe(1);
    expect(s.workspace.pchFiles()).toEqual(built);
});

/// A preamble with errors keeps its PCH, which body edits reuse. The
/// missing header showing up is a new input.
const generated = (n: number) =>
    `#include "generated.h"\nint main() { return generated() + ${n}; }\n`;

serve.files({ "main.cpp": generated(0) })("pch with errors waits for its inputs", async ({ s }) => {
    expect(await s.errors("main.cpp")).not.toEqual([]);
    for (let n = 1; n <= 3; n++) {
        s.edit("main.cpp", { text: generated(n) });
        await s.diagnostics("main.cpp");
    }
    expect((await s.counts()).pch).toBe(1);

    s.disk.write("generated.h", "#pragma once\ninline int generated() { return 1; }\n");
    await s.sync({ poll: true });
    await s.clean("main.cpp");
    expect((await s.counts()).pch).toBe(2);
});

/// A header that shows up in a search directory missing at the build is no
/// input the PCH recorded: a save retries a preamble that had errors.
serve.files(
    { "main.cpp": '#include "generated.h"\nint main() { return generated(); }\n' },
    { manifest: { cxx: ["-std=c++17"], units: { "main.cpp": ["-I${workspace}/gen"] } } },
)("pch with errors retries on save", async ({ s }) => {
    expect(await s.errors("main.cpp")).not.toEqual([]);

    s.disk.write("gen/generated.h", "#pragma once\ninline int generated() { return 1; }\n");
    s.save("main.cpp");
    await s.clean("main.cpp");
    expect(s.workspace.pchFiles()).toHaveLength(1);
});

const test = serve("shapes/headers");

test("pch diagnostics on open", async ({ s }) => {
    await s.clean(s.file("main"));
    expect((await s.counts()).files[s.file("main")]?.publish).toBeGreaterThan(0);
    s.close(s.file("main"));
});

test("pch body edit triggers recompile", async ({ s }) => {
    await s.compiled(s.file("registry"));
    s.edit(s.file("registry"), { replace: "return registered;", with: "return registered + 1;" });
    await s.diagnostics(s.file("registry"));
    expect((await s.counts()).files[s.file("registry")]).toMatchObject({ compile: 2, pch: 1 });
    s.close(s.file("registry"));
});

serve("tiny")("no pch for no includes", async ({ s }) => {
    await s.clean("main.cpp");
    const counts = await s.counts();
    expect(counts.files["main.cpp"]?.publish).toBeGreaterThan(0);
    expect(counts.pch).toBe(0);
    s.close("main.cpp");
});

test("hover on local symbol", async ({ s }) => {
    await s.compiled(s.file("registry"));
    expect(await s.hover(at(s.file("registry"), "int |registry_count() {"))).not.toBeNull();
    s.close(s.file("registry"));
});

test("completion with pch", async ({ s }) => {
    await s.compiled(s.file("main"));
    s.edit(s.file("main"), { replace: "shapes::Circle c(2.0);", with: "shapes::Circ" });
    expect(await s.completion(at(s.file("main"), "shapes::Circ|"))).not.toBeNull();
    s.close(s.file("main"));
});

test("preamble edit then hover", async ({ s }) => {
    await s.clean(s.file("registry"));
    // A project-local header, not a system one (<cstdio>): the PCH rebuild
    // stays fast on macOS CI.
    s.edit(s.file("registry"), {
        replace: '#include "shapes/registry.h"\n',
        with: '#include "shapes/registry.h"\n#include "shapes/registry.h"\n',
    });
    expect(await s.errors(s.file("registry")), "Expected no errors after preamble edit").toEqual(
        [],
    );
    expect(
        await s.hover(at(s.file("registry"), "int |registry_count() {")),
        "Hover failed after preamble edit",
    ).not.toBeNull();
    s.close(s.file("registry"));
});

test("preamble edit multiple times", async ({ s }) => {
    await s.compiled(s.file("registry"));
    for (let i = 0; i < 3; i++) {
        s.edit(s.file("registry"), { before: "\nnamespace shapes {", insert: `// edit ${i}\n` });
        await s.diagnostics(s.file("registry"));
    }
    expect(
        await s.errors(s.file("registry")),
        "Expected no errors after multiple preamble edits",
    ).toEqual([]);
    s.close(s.file("registry"));
});
