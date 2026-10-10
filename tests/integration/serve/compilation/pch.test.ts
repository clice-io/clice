/// PCH (precompiled header) builds of a unit's preamble: when one is built,
/// reused and built again.

import { at, expect, serve } from "../../fixtures.ts";

serve.files({ "main.cpp": "#include <iostream>\nint main() { return 0; }\n" })(
    "unchanged preamble keeps its pch",
    async ({ s }) => {
        // The standard library's lookups (`#include_next`, `__has_include`)
        // fail in some directories on the way: none of that is a change.
        await s.clean("main.cpp");
        const built = s.workspace.pchFiles();
        expect(built).toHaveLength(1);
        await s.completion(at("main.cpp", "|int main"));
        await s.sync();
        expect((await s.counts()).pch, "the pch was rebuilt").toBe(1);
        expect(s.workspace.pchFiles()).toEqual(built);
    },
);

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

const test = serve.data("pch_test");

test("pch diagnostics on open", async ({ s }) => {
    await s.clean("main.cpp");
    expect((await s.counts()).files["main.cpp"]?.publish).toBeGreaterThan(0);
    s.close("main.cpp");
});

test("pch body edit triggers recompile", async ({ s }) => {
    await s.compiled("main.cpp");
    s.edit("main.cpp", { replace: "return result;", with: "return result + 1;" });
    await s.diagnostics("main.cpp");
    expect((await s.counts()).files["main.cpp"]).toMatchObject({ compile: 2, pch: 1 });
    s.close("main.cpp");
});

test("no pch for no includes", async ({ s }) => {
    await s.clean("no_includes.cpp");
    const counts = await s.counts();
    expect(counts.files["no_includes.cpp"]?.publish).toBeGreaterThan(0);
    expect(counts.pch).toBe(0);
    s.close("no_includes.cpp");
});

test("hover on local symbol", async ({ s }) => {
    await s.compiled("main.cpp");
    expect(await s.hover(at("main.cpp", "int |add(int a, int b) {"))).not.toBeNull();
    s.close("main.cpp");
});

test("completion with pch", async ({ s }) => {
    await s.compiled("main.cpp");
    s.edit("main.cpp", { text: s.disk.read("main.cpp") + "\nPoi" });
    expect(await s.completion(at("main.cpp", "\nPoi|"))).not.toBeNull();
    s.close("main.cpp");
});

test("preamble edit then hover", async ({ s }) => {
    await s.clean("main.cpp");
    // A project-local header, not a system one (<cstdio>): the PCH rebuild
    // stays fast on macOS CI.
    s.edit("main.cpp", {
        replace: '#include "common.h"\n',
        with: '#include "common.h"\n#include "common.h"\n',
    });
    expect(await s.errors("main.cpp"), "Expected no errors after preamble edit").toEqual([]);
    expect(
        await s.hover(at("main.cpp", "int |add(int a, int b) {")),
        "Hover failed after preamble edit",
    ).not.toBeNull();
    s.close("main.cpp");
});

test("preamble edit multiple times", async ({ s }) => {
    await s.compiled("main.cpp");
    const body = s.disk.read("main.cpp").split("\n").slice(1).join("\n");
    for (let i = 0; i < 3; i++) {
        let includes = '#include "common.h"\n';
        for (let j = 0; j < i + 1; j++) {
            includes += `// edit ${j}\n`;
        }
        s.edit("main.cpp", { text: includes + body });
        await s.diagnostics("main.cpp");
    }
    expect(await s.errors("main.cpp"), "Expected no errors after multiple preamble edits").toEqual(
        [],
    );
    s.close("main.cpp");
});
