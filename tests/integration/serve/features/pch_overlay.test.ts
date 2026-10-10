/// Integration tests for the PCH overlay: header symbols of open in-memory
/// files resolve through the PCH's paired index blob, independent of the disk
/// index and faithful to the live buffer's preprocessor context.

import * as fs from "node:fs";
import * as proto from "vscode-languageserver-protocol";
import { MTIME_GRANULARITY } from "@clice/tools/client";
import { at, expect, serve } from "../../fixtures.ts";

const NO_INDEXING = { config: { project: { enable_indexing: false } } };
const FOO = { "foo.h": "inline void foo() {}\n" };
const MAIN = { "main.cpp": '#include "foo.h"\nint main() { foo(); return 0; }\n' };
const CALL = at("main.cpp", "foo();");

serve.files({ ...FOO, ...MAIN }, NO_INDEXING)("definition into unindexed header", async ({ s }) => {
    await s.compiled("main.cpp");
    // With background indexing off, only the PCH overlay knows the header.
    expect(s.show(await s.definition(CALL))).toBe("foo.h: inline void foo() {}");
});

serve.files(
    { "foo.h": "inline void foo() {}\ninline void bar() { foo(); }\n", ...MAIN },
    NO_INDEXING,
)("references include header rows", async ({ s }) => {
    await s.compiled("main.cpp");
    expect(
        s
            .show(await s.references(CALL))
            .split("\n")
            .sort(),
    ).toEqual([
        "foo.h: inline void bar() { foo(); }",
        "foo.h: inline void foo() {}",
        "main.cpp: int main() { foo(); return 0; }",
    ]);
});

serve.files({
    "crypto.h": "#ifdef USE_A\ninline void only_a() {}\n#else\ninline void only_b() {}\n#endif\n",
    "main.cpp": '#include "crypto.h"\nint main() { only_b(); return 0; }\n',
})("buffer context overrides disk", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.indexed();

    // The buffer's preamble now activates the branch no disk context has
    // ever seen; only the rebuilt PCH's overlay can resolve only_a.
    s.edit("main.cpp", {
        text: '#define USE_A 1\n#include "crypto.h"\nint main() { only_a(); return 0; }\n',
    });
    await s.compiled("main.cpp");

    expect(s.show(await s.definition(at("main.cpp", "only_a();")))).toBe(
        "crypto.h: inline void only_a() {}",
    );
});

serve.files({
    "foo.h": "inline void foo() {}\ninline void bar() { foo(); }\n",
    ...MAIN,
    "other.cpp": '#include "foo.h"\nint other() { return 0; }\n',
})("no duplicate reference rows", async ({ s }) => {
    await s.compiled("main.cpp");
    // Open files are skipped by background indexing; the closed other.cpp
    // is what carries foo.h's rows into the disk index.
    await s.indexed();

    // The header's rows exist in both its disk shard and the overlay; the
    // union must collapse them.
    const refs = (await s.references(CALL)) ?? [];
    const keys = refs.map((r) => `${r.uri}:${r.range.start.line}:${r.range.start.character}`);
    expect(keys.length, `duplicate reference rows: ${JSON.stringify(keys)}`).toBe(
        new Set(keys).size,
    );
    expect(refs.some((r) => r.uri.endsWith("foo.h"))).toBe(true);
});

serve.files({ "main.cpp": "#define ANSWER 42\nint main() { return ANSWER; }\n" }, NO_INDEXING)(
    "preamble macro definition",
    async ({ s }) => {
        await s.compiled("main.cpp");
        // The #define lives in the preamble region swallowed by the PCH; its
        // definition is served from the overlay's main-file entry.
        expect(s.show(await s.definition(at("main.cpp", "ANSWER; }")))).toBe(
            "main.cpp: #define ANSWER 42",
        );
    },
);

serve.files({ ...FOO, ...MAIN }, NO_INDEXING)("preamble include hover", async ({ s }) => {
    await s.compiled("main.cpp");
    // The include lives in the preamble, invisible to the worker's AST:
    // the hover must be served from the PCH's stored links.
    const hover = await s.hover(at("main.cpp", '"fo|o.h"'));
    expect(hover).not.toBeNull();
    expect(JSON.stringify(hover!.contents)).toContain("foo.h");
    // Link ranges are half-open: just past the closing quote hovers nothing.
    expect(await s.hover(at("main.cpp", '"foo.h"|\n'))).toBeNull();
});

serve.files(
    {
        ...FOO,
        "main.cpp":
            'int include(int value);\n#include "foo.h"\nint use(int value) { return include(value); }\n',
    },
    NO_INDEXING,
)("include hover past the preamble", async ({ s }) => {
    await s.compiled("main.cpp");
    const hover = await s.hover(at("main.cpp", '"fo|o.h"'));
    expect(JSON.stringify(hover?.contents)).toContain(s.workspace.displayPath("foo.h"));
    expect(
        await s.hover(at("main.cpp", "include(va|lue)")),
        "a call named include is no directive",
    ).not.toBeNull();
});

function linksFoo(links: unknown): boolean {
    return ((links as proto.DocumentLink[] | null) ?? []).some((link) =>
        (link.target ?? "").endsWith("foo.h"),
    );
}

serve.files({ ...FOO, ...MAIN })("preamble links survive restart", async ({ s }) => {
    await s.compiled("main.cpp");
    expect(linksFoo(await s.request("textDocument/documentLink", "main.cpp"))).toBe(true);
    const pchMtime = fs.statSync(s.workspace.pchFiles()[0]!).mtimeMs;

    // The next server hits the persisted PCH pair: the preamble's links must
    // be served from the reloaded blob, not lost with the process.
    await s.restart();
    await s.compiled("main.cpp");
    expect(
        linksFoo(await s.request("textDocument/documentLink", "main.cpp")),
        "preamble document links lost across restart",
    ).toBe(true);
    expect(
        fs.statSync(s.workspace.pchFiles()[0]!).mtimeMs,
        "PCH was rebuilt instead of reused",
    ).toBe(pchMtime);
});

serve.files({ ...FOO, ...MAIN })("missing idx rebuilds pair", async ({ s }) => {
    await s.compiled("main.cpp");
    expect(s.show(await s.definition(CALL))).toBe("foo.h: inline void foo() {}");
    const pch = s.workspace.pchFiles()[0]!;
    // The old PCH is set back past a filesystem's clock tick, so a rebuilt
    // one carries another mtime.
    const pchMtime = new Date(s.disk.mtime(pch).getTime() - MTIME_GRANULARITY);

    // Half the pair vanishes (crash residue, external cleanup): the next
    // server must treat the PCH as a miss and rebuild both blobs.
    await s.offline(() => {
        const idxFiles = s.workspace.pchIdxFiles();
        expect(idxFiles.length, "expected a committed .pch.idx next to the PCH").toBeGreaterThan(0);
        for (const idx of idxFiles) {
            fs.rmSync(idx);
        }
        s.disk.touch(pch, pchMtime);
    });
    await s.compiled("main.cpp");
    expect(
        s.show(await s.definition(CALL)),
        "overlay dead after losing the idx half of the pair",
    ).toBe("foo.h: inline void foo() {}");
    expect(
        fs.statSync(s.workspace.pchFiles()[0]!).mtimeMs,
        "PCH pair should have been rebuilt",
    ).not.toBe(pchMtime.getTime());
    expect(
        s.workspace.pchIdxFiles().length,
        "rebuilt pair is missing its idx blob",
    ).toBeGreaterThan(0);
});

serve.files({ ...FOO, ...MAIN }, NO_INDEXING)("header edit refreshes overlay", async ({ s }) => {
    await s.compiled("main.cpp");
    expect(s.show(await s.definition(CALL))).toBe("foo.h: inline void foo() {}");

    // Same preamble text, so the PCH key is unchanged; the header edit must
    // still refresh the pair (deps_changed) and the served overlay with it.
    s.disk.write("foo.h", "// moved\ninline void foo() {}\n");
    await s.sync({ poll: true });
    await s.compiled("main.cpp");

    // The definition moved to line 1, below the comment.
    expect(s.show(await s.definition(CALL))).toBe("foo.h: inline void foo() {}");
});
