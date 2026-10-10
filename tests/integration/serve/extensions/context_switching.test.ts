/// Integration tests for compilation context switching.
///
/// Covers source files with multiple CDB entries (selected by command hash),
/// include-occurrence contexts for guard-less headers, context deduplication
/// by canonical flags, host ranking, and switch validation.

import * as fs from "node:fs";
import { expect, serve } from "../../fixtures.ts";

const CXX17 = ["-std=c++17"];

/// Snapshot files as path -> mtime (nanoseconds).
function snapshotMtimes(files: string[]): Record<string, bigint> {
    const out: Record<string, bigint> = {};
    for (const file of files) {
        out[file] = fs.statSync(file, { bigint: true }).mtimeNs;
    }
    return out;
}

function messages(diagnostics: { message: string | { value: string } }[]): string[] {
    return diagnostics.map((d) => (typeof d.message === "string" ? d.message : d.message.value));
}

/// Switching between two CDB entries of one source must recompile it
/// under the selected flags. The unswitched default is content-decided
/// (not CDB order), so the test drives both states explicitly.
serve.files(
    { "main.cpp": "#ifndef EXPECTED\n#error missing EXPECTED\n#endif\nint main() { return 0; }\n" },
    { manifest: { cxx: CXX17, units: { "main.cpp": [["-DEXPECTED"], []] } } },
)("source command switch", async ({ s }) => {
    await s.compiled("main.cpp");

    const query = await s.contexts("main.cpp");
    expect(query.total).toBe(2);
    const contexts = query.contexts;
    expect(
        contexts.every((c) => c.commandHash!.length > 0),
        `Source contexts must carry commandHash, got: ${JSON.stringify(contexts)}`,
    ).toBe(true);
    const plainHash = contexts.find((c) => !c.label.includes("-DEXPECTED"))!.commandHash!;
    const definedHash = contexts.find((c) => c.label.includes("-DEXPECTED"))!.commandHash!;

    // Pin the entry with the define: clean compile.
    let switched = await s.switchContext("main.cpp", "main.cpp", { commandHash: definedHash });
    expect(switched.success).toBe(true);
    await s.clean("main.cpp");

    // Switch to the entry without the define: the #error must fire.
    switched = await s.switchContext("main.cpp", "main.cpp", { commandHash: plainHash });
    expect(switched.success).toBe(true);
    expect(
        (await s.errors("main.cpp")).length,
        "Expected #error without -DEXPECTED",
    ).toBeGreaterThan(0);

    const current = await s.currentContext("main.cpp");
    expect(current.context!.commandHash).toBe(plainHash);

    // And back.
    switched = await s.switchContext("main.cpp", "main.cpp", { commandHash: definedHash });
    expect(switched.success).toBe(true);
    await s.clean("main.cpp");
});

const OCCURRENCES = {
    "list.def": "X(alpha)\n",
    "main.cpp":
        "#define X(name) int name;\n" +
        '#include "list.def"\n' +
        "#undef X\n" +
        "#define X(name) void get_##name();\n" +
        '#include "list.def"\n' +
        "#undef X\n" +
        "int main() { return alpha; }\n",
};

/// A guard-less header included twice by one host provides one context
/// per include occurrence.
serve.files(OCCURRENCES)("occurrence switch", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.compiled("list.def");

    const query = await s.contexts("list.def");
    expect(query.total, `Expected 2 occurrence contexts, got ${JSON.stringify(query)}`).toBe(2);
    const occurrences = query.contexts.map((c) => c.occurrence).sort((a, b) => (a ?? 0) - (b ?? 0));
    expect(
        occurrences,
        `Expected occurrences 0 and 1, got: ${JSON.stringify(query.contexts)}`,
    ).toEqual([0, 1]);

    // Pin each occurrence; both must compile cleanly and be reported back.
    for (const occ of [0, 1]) {
        const switched = await s.switchContext("list.def", "main.cpp", { occurrence: occ });
        expect(switched.success, `switch to occurrence ${occ}`).toBe(true);
        await s.clean("list.def");

        const current = await s.currentContext("list.def");
        expect(current.context!.occurrence).toBe(occ);
        expect(current.automatic).toBe(false);
    }
});

/// Hosts with identical canonical flags collapse into one context, and
/// the representative is the best-ranked host (matching stem wins).
serve.files({
    "widget.h": "inline int widget_size() { return 4; }\n",
    "zzz.cpp": '#include "widget.h"\nint z() { return widget_size(); }\n',
    "widget.cpp": '#include "widget.h"\nint w() { return widget_size(); }\n',
    "aaa.cpp": '#include "widget.h"\nint a() { return widget_size(); }\n',
})("context dedup and ranking", async ({ s }) => {
    await s.compiled("widget.cpp");
    // Dedup requires a confirmed self-contained verdict, earned by the
    // header's own trial compile — wait for it.
    await s.compiled("widget.h");

    const query = await s.contexts("widget.h");
    expect(
        query.total,
        `Identical flags must dedupe to one context, got: ${JSON.stringify(query)}`,
    ).toBe(1);
    expect(
        query.contexts[0]!.uri.includes("widget.cpp"),
        `Representative should be the stem-matching host, got: ${JSON.stringify(query.contexts)}`,
    ).toBe(true);
});

/// Switching a header to a source that does not include it must fail.
serve.files({
    "utils.h": "inline int util() { return 1; }\n",
    "main.cpp": '#include "utils.h"\nint main() { return util(); }\n',
    "other.cpp": "int other() { return 2; }\n",
})("switch rejects non includer", async ({ s }) => {
    await s.compiled("main.cpp");
    s.open("utils.h");

    const switched = await s.switchContext("utils.h", "other.cpp");
    expect(switched.success, "Switching to a non-including host must be rejected").toBe(false);
});

/// Pinning an occurrence beyond the include count must be rejected.
serve.files({
    "list.def": "X(alpha)\n",
    "main.cpp":
        "#define X(name) int name;\n" +
        '#include "list.def"\n' +
        "#undef X\n" +
        "int main() { return alpha; }\n",
})("occurrence out of range", async ({ s }) => {
    await s.compiled("main.cpp");
    s.open("list.def");

    const switched = await s.switchContext("list.def", "main.cpp", { occurrence: 5 });
    expect(switched.success, "Out-of-range occurrence must be rejected").toBe(false);
});

const FLAVORS = Array.from({ length: 12 }, (_, n) => `s${String(n).padStart(2, "0")}.cpp`);

/// queryContext pages results: 12 distinct configs yield 10 + 2.
serve.files(
    {
        "common.h": "inline int common() { return 1; }\n",
        ...Object.fromEntries(
            FLAVORS.map((name, n) => [
                name,
                `#include "common.h"\nint f${n}() { return common() + FLAVOR; }\n`,
            ]),
        ),
    },
    {
        manifest: {
            cxx: ["-std=c++23"],
            units: Object.fromEntries(FLAVORS.map((name, n) => [name, [`-DFLAVOR=${n}`]])),
        },
    },
)("query context pagination", async ({ s }) => {
    await s.compiled("s00.cpp");
    s.open("common.h");

    const first = await s.contexts("common.h");
    expect(first.total).toBe(12);
    expect(first.contexts.length).toBe(10);

    const second = await s.contexts("common.h", { offset: 10 });
    expect(second.total).toBe(12);
    expect(second.contexts.length).toBe(2);

    // The two pages must not overlap.
    const firstUris = new Set(first.contexts.map((c) => c.uri));
    const secondUris = second.contexts.map((c) => c.uri);
    expect(secondUris.some((u) => firstUris.has(u))).toBe(false);
});

/// A switch made against an outdated queryContext listing is rejected
/// with stale=true; re-querying yields a fresh epoch that works.
serve.files({
    "shared.h": "VALUE_TYPE get_value();\n",
    "main.cpp": '#define VALUE_TYPE int\n#include "shared.h"\nint main() { return 0; }\n',
})("stale epoch rejected", async ({ s }) => {
    await s.compiled("main.cpp");
    s.open("shared.h");

    const query = await s.contexts("shared.h");
    const oldEpoch = query.epoch;
    expect(
        oldEpoch,
        `queryContext must stamp an epoch, got: ${JSON.stringify(query)}`,
    ).toBeTruthy();

    // A save of new bytes bumps the project epoch.
    s.edit("main.cpp", { replace: "return 0;", with: "return 1;" });
    s.save("main.cpp");
    await s.sync();

    let switched = await s.switchContext("shared.h", "main.cpp", { epoch: oldEpoch });
    expect(switched.success).toBe(false);
    expect(switched.stale, `Expected stale rejection, got: ${JSON.stringify(switched)}`).toBe(true);

    const fresh = await s.contexts("shared.h");
    switched = await s.switchContext("shared.h", "main.cpp", { epoch: fresh.epoch });
    expect(switched.success, `Fresh epoch must work, got: ${JSON.stringify(switched)}`).toBe(true);
});

/// A host built under several configurations provides one context per
/// CDB entry, switchable by command hash.
serve.files(
    {
        "render.h":
            "#pragma once\n" +
            "#if defined(USE_VULKAN)\n" +
            'inline const char* backend() { return "vk"; }\n' +
            "#elif defined(USE_METAL)\n" +
            'inline const char* backend() { return "mt"; }\n' +
            "#endif\n",
        "host.cpp": '#include "render.h"\nint main() { return backend()[0]; }\n',
    },
    { manifest: { cxx: CXX17, units: { "host.cpp": [["-DUSE_VULKAN"], ["-DUSE_METAL"]] } } },
)("multi config host", async ({ s }) => {
    await s.compiled("host.cpp");
    await s.compiled("render.h");

    const query = await s.contexts("render.h");
    expect(query.total, JSON.stringify(query)).toBe(2);
    const contexts = query.contexts;
    const hashes = contexts.map((c) => c.commandHash);
    expect(
        hashes.every((h) => h !== undefined && h.length > 0) && new Set(hashes).size === 2,
        JSON.stringify(contexts),
    ).toBe(true);

    const metalHash = contexts.find((c) => c.label.includes("USE_METAL"))!.commandHash!;
    const switched = await s.switchContext("render.h", "host.cpp", { commandHash: metalHash });
    expect(switched.success, JSON.stringify(switched)).toBe(true);

    await s.clean("render.h");
    const current = await s.currentContext("render.h");
    expect(current.context!.commandHash, JSON.stringify(current)).toBe(metalHash);
});

/// A file opened through a symlink is offered the commands of its identity:
/// the rules matching the file it names edit them.
serve
    .files(
        {
            "real/main.cpp": "int main() { return 0; }\n",
            "clice.toml": '[[rules]]\npatterns = ["real/**"]\nappend = ["-DFROM_RULE"]\n',
        },
        {
            manifest: { cxx: CXX17, units: { "real/main.cpp": [["-DFIRST"], ["-DSECOND"]] } },
            setup: (workspace) => {
                fs.symlinkSync(workspace.path("real"), workspace.path("link"));
            },
        },
    )
    .skipIf(process.platform === "win32")("contexts through a symlink", async ({ s }) => {
    await s.compiled("link/main.cpp");
    const labels = (await s.contexts("link/main.cpp")).contexts.map((c) => c.label);
    expect(labels).toHaveLength(2);
    for (const label of labels) {
        expect(label).toContain("FROM_RULE");
    }
});

/// Adding an #include and saving must immediately expose the new host
/// in queryContext: the include graph is rescanned on didSave.
serve.files({
    "lonely.h": "inline int lonely() { return 1; }\n",
    "main.cpp": "int main() { return 0; }\n",
})("saved include updates hosts", async ({ s }) => {
    await s.compiled("main.cpp");
    s.open("lonely.h");

    let query = await s.contexts("lonely.h");
    expect(query.total, "No includers yet").toBe(0);

    // Include the header and save.
    s.edit("main.cpp", { text: '#include "lonely.h"\nint main() { return lonely(); }\n' });
    s.save("main.cpp");

    query = await s.contexts("lonely.h");
    expect(query.total, `New host must appear after save: ${JSON.stringify(query)}`).toBe(1);
    expect(query.contexts[0]!.uri).toContain("main.cpp");
});

const CHAIN = {
    "list.def": "X(alpha)\nX(beta)\n",
    "main.cpp":
        "#define X(name) int name = 1;\n" +
        '#include "list.def"\n' +
        "#undef X\n" +
        "#define X(name) void get_##name();\n" +
        '#include "list.def"\n' +
        "#undef X\n" +
        "int main() { return alpha; }\n",
};

/// Closing and reopening a header keeps its context choice and reuses
/// the PCH built over its synthesized context.
serve.files(CHAIN)("reopen reuses preamble", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.compiled("list.def");

    const switched = await s.switchContext("list.def", "main.cpp", { occurrence: 1 });
    expect(switched.success).toBe(true);
    await s.diagnostics("list.def");

    const snapshot = snapshotMtimes(s.workspace.pchFiles());
    expect(Object.keys(snapshot).length, "expected a PCH over the context").toBeGreaterThan(0);
    const built = (await s.counts()).pch;

    s.close("list.def");
    await s.compiled("list.def");
    const current = await s.currentContext("list.def");
    expect(current.context!.occurrence).toBe(1);

    expect((await s.counts()).pch, "reopen must reuse the PCH, not rebuild it").toBe(built);
    expect(snapshotMtimes(s.workspace.pchFiles())).toEqual(snapshot);
});

/// Reopening a header after its chain file changed on disk must NOT
/// reuse the stale context — the chain content is embedded in it.
serve.files(CHAIN)("chain change resynthesizes", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.compiled("list.def");

    const switched = await s.switchContext("list.def", "main.cpp", { occurrence: 1 });
    expect(switched.success).toBe(true);
    await s.clean("list.def");

    s.close("list.def");
    // The chain file (the includer) changes on disk while the header is
    // closed: the embedded context content is now stale.
    s.disk.edit("main.cpp", {
        replace: "void get_##name();",
        with: "int get_##name = missing_value;",
    });

    const errors = messages(await s.errors("list.def"));
    const current = await s.currentContext("list.def");
    expect(current.context!.occurrence).toBe(1);
    expect(
        errors.some((m) => m.includes("missing_value")),
        `the reopened compile must see the new chain: ${JSON.stringify(errors)}`,
    ).toBe(true);
});

/// The client resync contract: after a successful switch the client
/// closes and reopens the document (the pull-based server only re-targets
/// the session); the reopened compile runs under the persisted choice.
serve.files(
    {
        "main.cpp":
            "#ifdef USE_B\nint broken() { return undefined_b_symbol; }\n#endif\n" +
            "int main() { return 0; }\n",
    },
    { manifest: { cxx: CXX17, units: { "main.cpp": [["-DUSE_A"], ["-DUSE_B"]] } } },
)("switched context survives reopen", async ({ s }) => {
    await s.compiled("main.cpp");

    const query = await s.contexts("main.cpp");
    const contexts = query.contexts;
    expect(query.total, `expected both entries: ${JSON.stringify(contexts)}`).toBe(2);
    const cleanHash = contexts.find((c) => (c.label || "").includes("USE_A"))!.commandHash!;
    const targetHash = contexts.find((c) => (c.label || "").includes("USE_B"))!.commandHash!;

    // The unswitched default is content-decided; pin USE_A for a known
    // starting state.
    let switched = await s.switchContext("main.cpp", "main.cpp", {
        commandHash: cleanHash,
        epoch: query.epoch,
    });
    expect(switched.success, `switch failed: ${JSON.stringify(switched)}`).toBe(true);
    await s.clean("main.cpp");

    switched = await s.switchContext("main.cpp", "main.cpp", { commandHash: targetHash });
    expect(switched.success, `switch failed: ${JSON.stringify(switched)}`).toBe(true);

    s.close("main.cpp");
    const diagnostics = await s.compiled("main.cpp");
    expect(
        messages(diagnostics).some((m) => m.includes("undefined_b_symbol")),
        `expected the USE_B error after reopen: ${JSON.stringify(diagnostics)}`,
    ).toBe(true);

    const current = await s.currentContext("main.cpp");
    expect(
        current.context!.commandHash,
        `persisted choice must survive the reopen: ${JSON.stringify(current)}`,
    ).toBe(targetHash);
});
