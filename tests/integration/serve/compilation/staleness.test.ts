/// Integration tests for mtime-based staleness tracking.
///
/// Verifies that ensure_compiled() and ensure_pch() detect dependency file
/// changes via mtime snapshots, triggering recompilation without relying
/// on didSave to mark everything dirty.

import { MTIME_GRANULARITY } from "@clice/tools/client";
import { at, expect, serve } from "../../fixtures.ts";

const MAIN_CALLS_VALUE = '#include "header.h"\nint main() { return value(); }\n';

const VALUE = {
    "header.h": "inline int value() { return 1; }\n",
    "main.cpp": MAIN_CALLS_VALUE,
};

const GUARDED_VALUE = {
    "header.h": "#pragma once\ninline int value() { return 1; }\n",
    "main.cpp": MAIN_CALLS_VALUE,
};

const MAIN_ONLY = { "main.cpp": "int main() { return 0; }\n" };

serve.files(VALUE)("header change invalidates ast", async ({ s }) => {
    // Modifying a header on disk should cause recompilation on the next
    // request, even though didSave was never called (mtime-based detection).
    await s.clean("main.cpp");

    s.disk.write("header.h", "inline int value() { return }\n"); // syntax error
    expect(
        (await s.errors("main.cpp")).length,
        "Expected diagnostics after header change",
    ).toBeGreaterThan(0);
});

serve.files({
    "header.h": "#pragma once\nstruct Foo { int x; };\n",
    "main.cpp": '#include "header.h"\nint main() { Foo f; return f.x; }\n',
})("header change invalidates pch", async ({ s }) => {
    // Modifying a preamble header on disk should trigger PCH rebuild.
    await s.clean("main.cpp");

    // The rewrite keeps the size: only a later mtime can tell it apart.
    s.disk.write("header.h", "#pragma once\nstruct Foo { int y; };\n"); // x -> y
    s.disk.touch("header.h", new Date(s.disk.mtime("header.h").getTime() + MTIME_GRANULARITY));

    // main.cpp uses f.x which no longer exists → diagnostics expected.
    expect(
        (await s.errors("main.cpp")).length,
        "Expected error after header field rename",
    ).toBeGreaterThan(0);
});

serve.files(MAIN_ONLY)("no change skips recompile", async ({ s }) => {
    // When no dependency has changed, ensure_compiled should fast-path.
    await s.clean("main.cpp");

    // "main" should be hoverable from the cached AST.
    expect(await s.hover(at("main.cpp", "main"))).not.toBeNull();
    expect((await s.counts()).files["main.cpp"]?.compile).toBe(1);
});

serve.files(VALUE)("touch without content change skips recompile", async ({ s }) => {
    // Layer 2: touching a header (mtime changes) without modifying content
    // should NOT trigger recompilation — the hash check catches this.
    await s.clean("main.cpp");

    // The touch must move the timestamps, or there is nothing to check.
    s.disk.write("header.h", s.disk.read("header.h"));
    s.disk.touch("header.h", new Date(s.disk.mtime("header.h").getTime() + MTIME_GRANULARITY));

    expect(await s.hover(at("main.cpp", "main"))).not.toBeNull();
    // No new diagnostics should appear — the file is still clean.
    await s.clean("main.cpp");
    expect((await s.counts()).files["main.cpp"]?.compile).toBe(1);
});

serve.files({
    "a.h": "#pragma once\nstruct Widget { int alpha_member; };\n",
    "main.cpp": '#include "a.h"\nint main() {\n    Widget w;\n    return 0;\n}\n',
})("touched pch input keeps completion", async ({ s }) => {
    // A same-bytes rewrite (git stash pop, a branch switch) moves only the
    // header's mtime: the PCH built from it must keep serving completion.
    await s.compiled("main.cpp");

    // The touch must move the timestamps, or there is nothing to check.
    s.disk.write("a.h", s.disk.read("a.h"));
    s.disk.touch("a.h", new Date(s.disk.mtime("a.h").getTime() + MTIME_GRANULARITY));

    s.edit("main.cpp", { before: "    return 0;", insert: "    w.\n" });
    const reply = await s.completion(at("main.cpp", "w.|"));
    const items = Array.isArray(reply) ? reply : (reply?.items ?? []);
    expect(items.map((item) => item.label.trim())).toContain("alpha_member");
});

serve.files(VALUE)("header replaced with different content", async ({ s }) => {
    // Replacing a header file with different content should be detected
    // and trigger recompilation reflecting the new content.
    await s.clean("main.cpp");

    // Replace header — delete and recreate with a breaking change.
    s.disk.rm("header.h");
    s.disk.write("header.h", "inline int renamed_value() { return 1; }\n");

    // main.cpp still calls value() which no longer exists → error.
    expect(
        (await s.errors("main.cpp")).length,
        "Expected diagnostics after header replacement",
    ).toBeGreaterThan(0);
});

serve.files({
    "header.h": "inline int value() { return }\n", // broken
    "main.cpp": MAIN_CALLS_VALUE,
})("fix error clears diagnostics", async ({ s }) => {
    // After introducing and fixing an error in a header, diagnostics
    // should clear on the next recompilation cycle.
    expect(
        (await s.errors("main.cpp")).length,
        "Expected diagnostics from broken header",
    ).toBeGreaterThan(0);

    s.disk.write("header.h", "inline int value() { return 1; }\n");
    expect(await s.recompiled("main.cpp")).toEqual([]);
});

serve.files({
    "shared.h": "inline int shared() { return 1; }\n",
    "a.cpp": '#include "shared.h"\nint fa() { return shared(); }\n',
    "b.cpp": '#include "shared.h"\nint fb() { return shared(); }\n',
})("multiple files share header", async ({ s }) => {
    // When a shared header changes, all open files that depend on it
    // should detect the staleness independently.
    await s.clean("a.cpp");
    await s.clean("b.cpp");

    // Break the shared header.
    s.disk.write("shared.h", "inline int shared() { return }\n");

    expect((await s.errors("a.cpp")).length, "File A should have diagnostics").toBeGreaterThan(0);
    expect((await s.errors("b.cpp")).length, "File B should have diagnostics").toBeGreaterThan(0);
});

serve.files({
    "base.h": "inline int base() { return 1; }\n",
    "mid.h": '#include "base.h"\n',
    "main.cpp": '#include "mid.h"\nint main() { return base(); }\n',
})("transitive header change", async ({ s }) => {
    // A change to a transitively included header should be detected.
    await s.clean("main.cpp");

    s.disk.write("base.h", "inline int base() { return }\n"); // broken
    expect(
        (await s.errors("main.cpp")).length,
        "Expected diagnostics from transitive header change",
    ).toBeGreaterThan(0);
});

serve.files(MAIN_ONLY)("didchange body edit recompiles", async ({ s }) => {
    // Editing the body (not preamble) via didChange should trigger
    // recompilation and update diagnostics.
    await s.clean("main.cpp");

    s.edit("main.cpp", { replace: "return 0; }", with: "return }" }); // missing expression
    expect(
        (await s.errors("main.cpp")).length,
        "Expected diagnostics after body error",
    ).toBeGreaterThan(0);
});

serve.files({
    "a.h": "#pragma once\ninline int from_a() { return 1; }\n",
    "b.h": "#pragma once\ninline int from_b() { return 2; }\n",
    "main.cpp": '#include "a.h"\nint main() { return from_a(); }\n',
})("didchange preamble edit recompiles", async ({ s }) => {
    // Changing a preamble #include via didChange should trigger PCH rebuild
    // and recompilation reflecting the new header's declarations.
    await s.clean("main.cpp");

    // Switch from a.h to b.h and call from_b() instead.
    s.edit("main.cpp", { replace: '"a.h"', with: '"b.h"' }, { replace: "from_a", with: "from_b" });

    // Should compile cleanly — from_b() is available via b.h.
    expect(await s.recompiled("main.cpp")).toEqual([]);
});

serve.files(MAIN_ONLY)("didclose then reopen", async ({ s }) => {
    // Closing and reopening a file should work correctly — the server
    // should not retain stale state from the previous session.
    await s.clean("main.cpp");

    s.close("main.cpp");
    // Modify on disk while closed.
    s.disk.write("main.cpp", "int main() { return }\n"); // broken

    // Reopen — should compile the new (broken) content from disk.
    expect(
        (await s.errors("main.cpp")).length,
        "Expected diagnostics after reopen with broken content",
    ).toBeGreaterThan(0);
});

serve.files(MAIN_ONLY)("didclose clears hover", async ({ s }) => {
    // After didClose, hover on the closed file should return an error.
    await s.compiled("main.cpp");

    s.close("main.cpp");
    await expect(s.hover(at("main.cpp", "main"))).rejects.toThrow("Document not open");
});

serve.files(VALUE)("didsave triggers recompile for dependents", async ({ s }) => {
    // didSave on a header file should mark dependent documents dirty.
    await s.clean("main.cpp");

    // Modify header on disk and send didSave; the header is not open.
    s.disk.write("header.h", "inline int value() { return }\n"); // broken
    s.save("header.h");

    expect(
        (await s.errors("main.cpp")).length,
        "Expected diagnostics after didSave on broken header",
    ).toBeGreaterThan(0);
});

serve.data("modules/save_recompile")("didsave with module deps", async ({ s }) => {
    // didSave on a module file should invalidate CompileGraph dependents.
    // Mid imports Leaf.
    await s.clean("mid.cppm");

    // Modify Leaf on disk and send didSave — should invalidate Mid's deps.
    s.disk.write("leaf.cppm", "export module Leaf;\nexport int leaf() { return 999; }\n");
    s.save("leaf.cppm");

    // Mid recompiles: the Leaf PCM was invalidated.
    expect(await s.recompiled("mid.cppm")).toEqual([]);
});

serve.files(
    {
        "header.h": "#pragma once\nstruct F { int x; };\n",
        "main.cpp": '#include "header.h"\nint main() { F f; return f.x; }\n',
    },
    { manifest: { units: { "main.cpp": ["-DFOO=1"] } } },
)("flag change invalidates pch", async ({ s }) => {
    // Changing a -D flag in the CDB must produce a new PCH on the next
    // session even though the preamble text is unchanged (flags are part of
    // the cache key).
    await s.clean("main.cpp");
    expect(s.workspace.pchFiles().length).toBe(1);
    await s.noAnomaly();

    // Same preamble text, different flag — must not reuse.
    await s.offline(() => {
        s.disk.edit("compile_commands.json", { replace: "-DFOO=1", with: "-DFOO=2" });
    });
    await s.clean("main.cpp");
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

serve.files(GUARDED_VALUE)("same second save detected", async ({ s }) => {
    // A header saved immediately after the dependent's compile — within the
    // same second — must still invalidate the PCH. Deliberately no mtime sleep:
    // a watermark-based freshness check is blind exactly here.
    await s.clean("main.cpp");

    s.disk.write("header.h", "#pragma once\ninline int renamed() { return 1; }\n");
    s.save("header.h");

    // main.cpp still calls value(): a reused stale PCH would compile clean.
    expect(
        (await s.errors("main.cpp")).length,
        "Expected errors after same-second header save",
    ).toBeGreaterThan(0);
});

serve.files(GUARDED_VALUE)("backdated header change detected", async ({ s }) => {
    // A header whose content changes while its mtime moves backwards
    // (rsync -t, git-restore-mtime) must be caught by the pull-side check
    // alone — no didSave is sent.
    await s.clean("main.cpp");

    const mtime = s.disk.mtime("header.h");
    s.disk.write("header.h", "#pragma once\ninline int renamed() { return 1; }\n");
    s.disk.touch("header.h", new Date(mtime.getTime() - 100_000));

    expect(
        (await s.errors("main.cpp")).length,
        "Expected errors after backdated header change",
    ).toBeGreaterThan(0);
});

serve.files(
    {
        "pre.h": "#pragma once\ninline int pre() { return 1; }\n",
        "late.h": "#pragma once\ninline int late() { return 1; }\n",
        "main.cpp":
            '#include "pre.h"\nint main() { return pre(); }\n#include "late.h"\nint tail() { return late(); }\n',
    },
    { config: { project: { enable_indexing: false } } },
)("outside edits compile once", async ({ s }) => {
    // Headers on both sides of the preamble change behind the server's back:
    // the request's own check finds both, and their cascade lands before
    // the recompile, not in the middle of it.
    await s.clean("main.cpp");

    s.disk.write("pre.h", "#pragma once\ninline int pre_renamed() { return 1; }\n");
    s.disk.write("late.h", "#pragma once\ninline int late_renamed() { return 1; }\n");
    const before = (await s.counts()).files["main.cpp"];
    const errors = await s.errors("main.cpp");
    const after = (await s.counts()).files["main.cpp"];
    expect((after?.publish ?? 0) - (before?.publish ?? 0)).toBe(1);
    expect((after?.compile ?? 0) - (before?.compile ?? 0)).toBe(1);
    expect(errors).toHaveLength(2);
});

serve.files({
    "main.cpp": "int main() { return 0; }\n",
    "orphan.h": "inline int orphan_value() { return 7; }\n",
})("orphan header default command", async ({ s }) => {
    // A header with no CDB entry and no including source falls back to the
    // synthesized default command and still compiles.
    await s.clean("orphan.h");
});

serve.files(MAIN_ONLY, {
    manifest: { units: { "main.cpp": ["--target=bogus-unknown-none"] } },
})("setup fail keeps dirty", async ({ s }) => {
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
