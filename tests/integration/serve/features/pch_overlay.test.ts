/// Integration tests for the PCH overlay: header symbols of open in-memory
/// files resolve through the PCH's paired index blob, independent of the disk
/// index and faithful to the live buffer's preprocessor context.

import * as fs from "node:fs";
import * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import { MTIME_GRANULARITY } from "@clice/tools/client";
import { at, expect, serve } from "../../fixtures.ts";

const NO_INDEXING = { config: { project: { enable_indexing: false } } };

/// Where the definition of detail::square, an inline function of a header
/// in circle.cpp's preamble, shows.
const SQUARE = "inline double square(double value) {";

serve("shapes/headers", NO_INDEXING)("definition into unindexed header", async ({ s }) => {
    await s.compiled(s.file("circle_impl"));
    // With background indexing off, only the PCH overlay knows the header.
    expect(s.show(await s.definition(at(s.file("circle_impl"), "squ|are(")))).toBe(
        `${s.file("detail")}: ${SQUARE}`,
    );
});

serve("shapes/headers", NO_INDEXING)("references include header rows", async ({ s }) => {
    await s.compiled(s.file("circle_impl"));
    expect(
        s
            .show(await s.references(at(s.file("circle_impl"), "circle.ra|dius")))
            .split("\n")
            .sort(),
    ).toEqual([
        `${s.file("circle")}: double radius;`,
        `${s.file("circle")}: explicit Circle(double radius) : radius(radius) {}`,
        `${s.file("circle_impl")}: return detail::scale(pi * detail::square(circle.radius));`,
    ]);
});

serve("shapes/headers")("buffer context overrides disk", async ({ s }) => {
    await s.compiled(s.file("main"));
    await s.indexed();

    // The buffer's preamble now activates config.h's SHAPES_TRACE branch,
    // which no unit's command defines and so no disk context has ever seen;
    // only the rebuilt PCH's overlay can resolve shapes_trace.
    s.edit(
        s.file("main"),
        { before: '#include "shapes/c_api.h"', insert: "#define SHAPES_TRACE\n" },
        { after: "int main() {", insert: "\n    shapes_trace();" },
    );
    await s.compiled(s.file("main"));

    expect(s.show(await s.definition(at(s.file("main"), "int main() {\n    |")))).toBe(
        `${s.file("config")}: inline void shapes_trace() {}`,
    );
});

serve("shapes/headers")("no duplicate reference rows", async ({ s }) => {
    await s.compiled(s.file("main"));
    // Open files are skipped by background indexing; the closed units that
    // include circle.h carry its rows into the disk index.
    await s.indexed();

    // The header's rows exist in both its disk shard and the overlay; the
    // union must collapse them.
    const refs = (await s.references(at(s.file("main"), "shapes::ar|ea(c)"))) ?? [];
    const keys = refs.map((r) => `${r.uri}:${r.range.start.line}:${r.range.start.character}`);
    expect(keys.length, `duplicate reference rows: ${JSON.stringify(keys)}`).toBe(
        new Set(keys).size,
    );
    expect(refs.some((r) => s.relative(r.uri) === s.file("circle"))).toBe(true);
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

serve("shapes/headers", NO_INDEXING)("preamble include hover", async ({ s }) => {
    await s.compiled(s.file("main"));
    // The include lives in the preamble, invisible to the worker's AST:
    // the hover must be served from the PCH's stored links.
    const hover = await s.hover(at(s.file("main"), '"shapes/cir|cle.h"'));
    expect(hover).not.toBeNull();
    expect(JSON.stringify(hover!.contents)).toContain("circle.h");
    // Link ranges are half-open: just past the closing quote hovers nothing.
    expect(await s.hover(at(s.file("main"), '"shapes/circle.h"|\n'))).toBeNull();
});

serve.files(
    {
        "foo.h": "inline void foo() {}\n",
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

/// The workspace files `links` point at.
function linked(s: Serve, links: unknown): string[] {
    return ((links as proto.DocumentLink[] | null) ?? []).flatMap((link) =>
        link.target === undefined ? [] : [s.relative(link.target)],
    );
}

serve("shapes/headers")("preamble links survive restart", async ({ s }) => {
    await s.compiled(s.file("main"));
    expect(linked(s, await s.request("textDocument/documentLink", s.file("main")))).toContain(
        s.file("circle"),
    );
    const pchMtime = fs.statSync(s.workspace.pchFiles()[0]!).mtimeMs;

    // The next server hits the persisted PCH pair: the preamble's links must
    // be served from the reloaded blob, not lost with the process.
    await s.restart();
    await s.compiled(s.file("main"));
    expect(
        linked(s, await s.request("textDocument/documentLink", s.file("main"))),
        "preamble document links lost across restart",
    ).toContain(s.file("circle"));
    expect(
        fs.statSync(s.workspace.pchFiles()[0]!).mtimeMs,
        "PCH was rebuilt instead of reused",
    ).toBe(pchMtime);
});

serve("shapes/headers", NO_INDEXING)("missing idx rebuilds pair", async ({ s }) => {
    await s.compiled(s.file("circle_impl"));
    expect(s.show(await s.definition(at(s.file("circle_impl"), "squ|are(")))).toBe(
        `${s.file("detail")}: ${SQUARE}`,
    );
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
    await s.compiled(s.file("circle_impl"));
    expect(
        s.show(await s.definition(at(s.file("circle_impl"), "squ|are("))),
        "overlay dead after losing the idx half of the pair",
    ).toBe(`${s.file("detail")}: ${SQUARE}`);
    expect(
        fs.statSync(s.workspace.pchFiles()[0]!).mtimeMs,
        "PCH pair should have been rebuilt",
    ).not.toBe(pchMtime.getTime());
    expect(
        s.workspace.pchIdxFiles().length,
        "rebuilt pair is missing its idx blob",
    ).toBeGreaterThan(0);
});

serve("shapes/headers", NO_INDEXING)("header edit refreshes overlay", async ({ s }) => {
    await s.compiled(s.file("circle_impl"));
    expect(s.show(await s.definition(at(s.file("circle_impl"), "squ|are(")))).toBe(
        `${s.file("detail")}: ${SQUARE}`,
    );

    // Same preamble text, so the PCH key is unchanged; the header edit must
    // still refresh the pair (deps_changed) and the served overlay with it.
    s.disk.edit(s.file("detail"), { before: "inline double square(", insert: "// moved\n" });
    await s.sync({ poll: true });
    await s.compiled(s.file("circle_impl"));

    // The definition moved a line down, below the comment.
    expect(s.show(await s.definition(at(s.file("circle_impl"), "squ|are(")))).toBe(
        `${s.file("detail")}: ${SQUARE}`,
    );
});
