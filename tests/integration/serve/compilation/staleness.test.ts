/// Integration tests for mtime-based staleness tracking.
///
/// Verifies that ensure_compiled() and ensure_pch() detect dependency file
/// changes via mtime snapshots, triggering recompilation without relying
/// on didSave to mark everything dirty.

import { MTIME_GRANULARITY } from "@clice/tools/client";
import { at, expect, serve } from "../../fixtures.ts";

serve("shapes/headers")("header change invalidates ast", async ({ s }) => {
    // Modifying a header on disk should cause recompilation on the next
    // request, even though didSave was never called (mtime-based detection).
    await s.clean(s.file("main"));

    s.disk.edit(s.file("circle"), { replace: "double radius;", with: "double radius" });
    expect(
        (await s.errors(s.file("main"))).length,
        "Expected diagnostics after header change",
    ).toBeGreaterThan(0);
});

serve("shapes/headers")("header change invalidates pch", async ({ s }) => {
    // Modifying a preamble header on disk should trigger PCH rebuild.
    await s.clean(s.file("main"));

    // The rewrite keeps the size: only a later mtime can tell it apart.
    s.disk.edit(s.file("circle"), { replace: "double area(", with: "double aera(" });
    s.disk.touch(
        s.file("circle"),
        new Date(s.disk.mtime(s.file("circle")).getTime() + MTIME_GRANULARITY),
    );

    // main.cpp calls area(), which no longer exists.
    expect(
        (await s.errors(s.file("main"))).length,
        "Expected error after header function rename",
    ).toBeGreaterThan(0);
});

serve("tiny")("no change skips recompile", async ({ s }) => {
    // When no dependency has changed, ensure_compiled should fast-path.
    await s.clean("main.cpp");

    // "main" should be hoverable from the cached AST.
    expect(await s.hover(at("main.cpp", "main"))).not.toBeNull();
    expect((await s.counts()).files["main.cpp"]?.compile).toBe(1);
});

serve("shapes/headers")("touch without content change skips recompile", async ({ s }) => {
    // Layer 2: touching a header (mtime changes) without modifying content
    // should NOT trigger recompilation — the hash check catches this.
    await s.clean(s.file("main"));

    // The touch must move the timestamps, or there is nothing to check.
    s.disk.write(s.file("circle"), s.disk.read(s.file("circle")));
    s.disk.touch(
        s.file("circle"),
        new Date(s.disk.mtime(s.file("circle")).getTime() + MTIME_GRANULARITY),
    );

    expect(await s.hover(at(s.file("main"), "main"))).not.toBeNull();
    // No new diagnostics should appear — the file is still clean.
    await s.clean(s.file("main"));
    expect((await s.counts()).files[s.file("main")]?.compile).toBe(1);
});

serve("shapes/headers")("touched pch input keeps completion", async ({ s }) => {
    // A same-bytes rewrite (git stash pop, a branch switch) moves only the
    // header's mtime: the PCH built from it must keep serving completion.
    await s.compiled(s.file("main"));

    // The touch must move the timestamps, or there is nothing to check.
    s.disk.write(s.file("circle"), s.disk.read(s.file("circle")));
    s.disk.touch(
        s.file("circle"),
        new Date(s.disk.mtime(s.file("circle")).getTime() + MTIME_GRANULARITY),
    );

    s.edit(s.file("main"), { before: "\n    return static_cast", insert: "\n    c." });
    const reply = await s.completion(at(s.file("main"), "|\n    return static_cast"));
    const items = Array.isArray(reply) ? reply : (reply?.items ?? []);
    expect(items.map((item) => item.label.trim())).toContain("radius");
});

serve("shapes/headers")("header replaced with different content", async ({ s }) => {
    // Replacing a header file with different content should be detected
    // and trigger recompilation reflecting the new content.
    await s.clean(s.file("main"));

    // Replace header — delete and recreate with a breaking change.
    const text = s.disk.read(s.file("circle"));
    s.disk.rm(s.file("circle"));
    s.disk.write(s.file("circle"), text.replace("double area(", "double surface("));

    // main.cpp still calls area(), which no longer exists → error.
    expect(
        (await s.errors(s.file("main"))).length,
        "Expected diagnostics after header replacement",
    ).toBeGreaterThan(0);
});

serve("shapes/headers")("fix error clears diagnostics", async ({ s }) => {
    // After introducing and fixing an error in a header, diagnostics
    // should clear on the next recompilation cycle.
    const intact = s.disk.read(s.file("circle"));
    s.disk.edit(s.file("circle"), { replace: "double area(", with: "double surface(" });
    expect(
        (await s.errors(s.file("main"))).length,
        "Expected diagnostics from broken header",
    ).toBeGreaterThan(0);

    s.disk.write(s.file("circle"), intact);
    expect(await s.recompiled(s.file("main"))).toEqual([]);
});

serve("shapes/headers")("multiple files share header", async ({ s }) => {
    // When a shared header changes, all open files that depend on it
    // should detect the staleness independently.
    await s.clean(s.file("main"));
    await s.clean(s.file("demo"));

    // Break the shared header.
    s.disk.edit(s.file("circle"), { replace: "double radius;", with: "double radius" });

    expect((await s.errors(s.file("main"))).length, "main has diagnostics").toBeGreaterThan(0);
    expect((await s.errors(s.file("demo"))).length, "demo has diagnostics").toBeGreaterThan(0);
});

serve("shapes/headers")("transitive header change", async ({ s }) => {
    // A change to a transitively included header should be detected.
    await s.clean(s.file("main"));

    s.disk.edit(s.file("units"), {
        replace: "#define SHAPES_PI 3.14159",
        with: "#define SHAPES_PI",
    });
    expect(
        (await s.errors(s.file("main"))).length,
        "Expected diagnostics from transitive header change",
    ).toBeGreaterThan(0);
});

serve("tiny")("didchange body edit recompiles", async ({ s }) => {
    // Editing the body (not preamble) via didChange should trigger
    // recompilation and update diagnostics.
    await s.clean("main.cpp");

    s.edit("main.cpp", { replace: "value - 3", with: "value -" }); // missing operand
    expect(
        (await s.errors("main.cpp")).length,
        "Expected diagnostics after body error",
    ).toBeGreaterThan(0);
});

serve("shapes/headers")("didchange preamble edit recompiles", async ({ s }) => {
    // Changing a preamble #include via didChange should trigger PCH rebuild
    // and recompilation reflecting the new header's declarations.
    await s.clean(s.file("main"));

    // Swap registry.h for draft.h, which no unit includes, and use it.
    s.edit(
        s.file("main"),
        { replace: '"shapes/registry.h"', with: '"shapes/draft.h"' },
        { replace: "shapes::registry_count()", with: "shapes::Draft{3}.sides" },
    );

    // Should compile cleanly — Draft is available via draft.h.
    expect(await s.recompiled(s.file("main"))).toEqual([]);
});

serve("tiny")("didclose then reopen", async ({ s }) => {
    // Closing and reopening a file should work correctly — the server
    // should not retain stale state from the previous session.
    await s.clean("main.cpp");

    s.close("main.cpp");
    // Modify on disk while closed.
    s.disk.edit("main.cpp", { replace: "value - 3", with: "value -" }); // broken

    // Reopen — should compile the new (broken) content from disk.
    expect(
        (await s.errors("main.cpp")).length,
        "Expected diagnostics after reopen with broken content",
    ).toBeGreaterThan(0);
});

serve("tiny")("didclose clears hover", async ({ s }) => {
    // After didClose, hover on the closed file should return an error.
    await s.compiled("main.cpp");

    s.close("main.cpp");
    await expect(s.hover(at("main.cpp", "main"))).rejects.toThrow("Document not open");
});

serve("shapes/headers")("didsave triggers recompile for dependents", async ({ s }) => {
    // didSave on a header file should mark dependent documents dirty.
    await s.clean(s.file("main"));

    // Modify header on disk and send didSave; the header is not open.
    s.disk.edit(s.file("circle"), { replace: "double radius;", with: "double radius" });
    s.save(s.file("circle"));

    expect(
        (await s.errors(s.file("main"))).length,
        "Expected diagnostics after didSave on broken header",
    ).toBeGreaterThan(0);
});

serve("shapes/modules")("didsave with module deps", async ({ s }) => {
    // didSave on a module file should invalidate CompileGraph dependents.
    // The circle partition imports the shape partition.
    await s.clean(s.file("circle"));

    // Modify shape on disk and send didSave — should invalidate circle's deps.
    s.disk.edit(s.file("shape"), {
        replace: "virtual ~Shape() = default;",
        with: "virtual ~Shape() {}",
    });
    s.save(s.file("shape"));

    // circle recompiles: the shape PCM was invalidated.
    expect(await s.recompiled(s.file("circle"))).toEqual([]);
});

serve("shapes/headers")("flag change invalidates pch", async ({ s }) => {
    // Changing a -D flag in the CDB must produce a new PCH on the next
    // session even though the preamble text is unchanged (flags are part of
    // the cache key).
    await s.clean(s.file("circle_impl"));
    expect(s.workspace.pchFiles().length).toBe(1);
    await s.noAnomaly();

    // Same preamble text, different flag — must not reuse.
    await s.offline(() => {
        s.disk.database({
            ...s.manifest,
            units: { ...s.manifest.units, [s.file("circle_impl")]: ["-DSHAPES_EXACT=2"] },
        });
    });
    await s.clean(s.file("circle_impl"));
    expect(
        s.workspace.pchFiles().length,
        "A flag change must produce a second, separately keyed PCH",
    ).toBe(2);
    await s.noAnomaly();
});

const POINT_HOST = {
    "types.h": "#pragma once\nstruct Point { int x; int y; };\n",
    "utils.h": "inline int get_x(Point p) { return p.x; }\n",
    "main.cpp":
        '#include "types.h"\n#include "utils.h"\n' +
        "int main() { Point p{1, 2}; return get_x(p); }\n",
};

serve.files(POINT_HOST)("host change resynthesizes preamble", async ({ s }) => {
    // When the host source stops providing a dependency, the header's
    // synthesized preamble must be rebuilt from the new disk state.
    await s.compiled("main.cpp");

    // utils.h has no CDB entry: compiled via automatic header context,
    // with types.h provided by the synthesized preamble from main.cpp.
    await s.clean("utils.h");

    // No didSave: mtime-based chain snapshot must detect the change.
    s.disk.write("main.cpp", '#include "utils.h"\nint main() { return 0; }\n');
    expect(
        (await s.errors("utils.h")).length,
        "Expected errors after host stopped providing types.h",
    ).toBeGreaterThan(0);
});

serve.files({
    "wrapper.h": '#pragma once\n#define VALUE 42\n#include "target.h"\n',
    "target.h": "inline int get() { return VALUE; }\n",
    "main.cpp": '#include "wrapper.h"\nint main() { return get(); }\n',
})("intermediate change resynthesizes preamble", async ({ s }) => {
    // Changing an intermediate file of the include chain (not the host)
    // must also invalidate the synthesized preamble.
    await s.compiled("main.cpp");

    // target.h compiles via chain main.cpp -> wrapper.h, which provides VALUE.
    await s.clean("target.h");

    // Rename the macro in the intermediate wrapper.h. The rewrite keeps the
    // size: only a later mtime can tell it apart.
    s.disk.write("wrapper.h", '#pragma once\n#define OTHER 42\n#include "target.h"\n');
    s.disk.touch("wrapper.h", new Date(s.disk.mtime("wrapper.h").getTime() + MTIME_GRANULARITY));

    expect(
        (await s.errors("target.h")).length,
        "Expected errors after intermediate header changed",
    ).toBeGreaterThan(0);
});

serve.files(POINT_HOST)("saved host reinvalidates header", async ({ s }) => {
    // didSave on a chain file must force preamble re-validation by content
    // even when the file's mtime is unchanged (the pull path is blind then).
    await s.compiled("main.cpp");
    await s.clean("utils.h");

    // Rewrite main.cpp but restore its original mtime: the mtime-based
    // Layer 1 check now cannot see the change, only the didSave push path
    // (which zeroes build_at, forcing a content re-hash) can catch it. The
    // buffer of main.cpp stays as it was.
    const mtime = s.disk.mtime("main.cpp");
    s.disk.write("main.cpp", '#include "utils.h"\nint main() { return 0; }\n');
    s.disk.touch("main.cpp", mtime);
    s.save("main.cpp", { write: false });

    expect(
        (await s.errors("utils.h")).length,
        "Expected errors after didSave with restored mtime",
    ).toBeGreaterThan(0);
});

serve("shapes/headers")("same second save detected", async ({ s }) => {
    // A header saved immediately after the dependent's compile — within the
    // same second — must still invalidate the PCH. Deliberately no mtime sleep:
    // a watermark-based freshness check is blind exactly here.
    await s.clean(s.file("main"));

    s.disk.edit(s.file("circle"), { replace: "double area(", with: "double surface(" });
    s.save(s.file("circle"));

    // main.cpp still calls area(): a reused stale PCH would compile clean.
    expect(
        (await s.errors(s.file("main"))).length,
        "Expected errors after same-second header save",
    ).toBeGreaterThan(0);
});

serve("shapes/headers")("backdated header change detected", async ({ s }) => {
    // A header whose content changes while its mtime moves backwards
    // (rsync -t, git-restore-mtime) must be caught by the pull-side check
    // alone — no didSave is sent.
    await s.clean(s.file("main"));

    const mtime = s.disk.mtime(s.file("circle"));
    s.disk.edit(s.file("circle"), { replace: "double area(", with: "double surface(" });
    s.disk.touch(s.file("circle"), new Date(mtime.getTime() - 100_000));

    expect(
        (await s.errors(s.file("main"))).length,
        "Expected errors after backdated header change",
    ).toBeGreaterThan(0);
});

serve("shapes/headers", { config: { project: { enable_indexing: false } } })(
    "outside edits compile once",
    async ({ s }) => {
        // Headers on both sides of the preamble change behind the server's
        // back: the request's own check finds both, and their cascade lands
        // before the recompile, not in the middle of it.
        await s.clean(s.file("demo"));

        s.disk.edit(
            s.file("circle"),
            { replace: "class UnitCircle", with: "class UnitDisk" },
            { replace: "UnitCircle()", with: "UnitDisk()" },
        );
        s.disk.edit(s.file("detail"), { replace: "double square(", with: "double squared(" });
        const before = (await s.counts()).files[s.file("demo")];
        const errors = await s.errors(s.file("demo"));
        const after = (await s.counts()).files[s.file("demo")];
        expect((after?.publish ?? 0) - (before?.publish ?? 0)).toBe(1);
        expect((after?.compile ?? 0) - (before?.compile ?? 0)).toBe(1);
        expect(errors).toHaveLength(2);
    },
);

serve("shapes/headers")("orphan header default command", async ({ s }) => {
    // A header with no CDB entry and no including source falls back to the
    // synthesized default command and still compiles.
    await s.clean(s.file("draft"));
});

const BOGUS_TARGET = { units: { "main.cpp": ["--target=bogus-unknown-none"] } };

serve("tiny", BOGUS_TARGET)("setup fail keeps dirty", async ({ s }) => {
    // A compile that fails before parsing (bad target triple, no PCH to
    // blame) must not settle: the gap is published as empty diagnostics and
    // the next request recompiles instead of trusting the phantom product.
    expect(await s.compiled("main.cpp"), "Honest gap must be empty").toEqual([]);

    // A settled phantom would serve the stale AST and never publish again;
    // a retained dirty flag recompiles and republishes on the next request.
    expect(
        await s.recompiled("main.cpp"),
        "The retried non-result must stay an honest empty gap",
    ).toEqual([]);
});
