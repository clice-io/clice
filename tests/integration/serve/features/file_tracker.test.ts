/// File tracker: each case drives deterministic ticks through the
/// clice/internal/poll hook (loops disabled), whose event counts it
/// asserts. A workspace tick looks at every known file; a look finding
/// other bytes than the one before — the scan's included — is a change.

import * as fs from "node:fs";
import { MTIME_GRANULARITY, sleep } from "@clice/tools/client";
import type { Serve, ServeOptions } from "@clice/tools/actions";
import { writeDatabase } from "@clice/tools/project";
import { at, expect, serve, type Loc } from "../../fixtures.ts";

const GATED_MAIN = `#ifndef FEATURE
#error missing FEATURE
#endif
int main() { return 0; }
`;

const HEADER_V1 = `#define VALUE 1
#define TARGET alpha
inline int alpha() { return 1; }
inline int beta() { return 2; }
`;

const HEADER_V2 = `#define VALUE 2
#define TARGET beta
inline int alpha() { return 1; }
inline int beta() { return 2; }
`;

const GATED_LIB = `#ifdef FEATURE
int feature_on() { return 1; }
#else
int feature_off() { return 0; }
#endif
`;

const CLOSED = '#include "header.h"\nint use_target() { return TARGET(); }\n';

const CXX = ["-std=c++17"];

function units(sources: string[], args: string[] = []): Record<string, string[]> {
    return Object.fromEntries(sources.map((source) => [source, args]));
}

/// Every source a C++17 unit with `args`, `options` besides.
function cxx17(sources: string[], args: string[] = [], options: ServeOptions = {}): ServeOptions {
    return { ...options, manifest: { cxx: CXX, units: units(sources, args) } };
}

/// Rewrite the database to name `entries`.
function database(s: Serve, entries: Record<string, string[]>): void {
    s.steps.note(`disk: database of ${Object.keys(entries).join(", ") || "no unit"}`);
    writeDatabase(s.workspace, { cxx: CXX, units: entries });
}

/// The database as `entries` name them while no server runs: its
/// arguments spell workspace paths, which a manifest cannot.
function offlineDatabase(s: Serve, entries: Record<string, string[]>): Promise<void> {
    return s.offline(() => {
        database(s, entries);
    });
}

async function events(
    s: Serve,
    loop: "cdb" | "workspace",
    options: { force?: boolean } = {},
): Promise<number> {
    return (await s.client.poll(loop, options)).events;
}

/// The files whose rows reference the symbol at `loc`.
async function referrers(s: Serve, loc: Loc): Promise<string[]> {
    return [...new Set(((await s.references(loc)) ?? []).map((site) => s.relative(site.uri)))];
}

async function indexes(s: Serve, name: string): Promise<boolean> {
    const symbols = (await s.workspaceSymbols(name)) ?? [];
    return symbols.some((symbol) => symbol.name === name);
}

const ALPHA = at("header.h", "alpha()");
const BETA = at("header.h", "beta()");

serve.files({ "main.cpp": GATED_MAIN }, cxx17(["main.cpp"]))(
    "cdb flag change recompiles",
    async ({ s }) => {
        expect(await s.errors("main.cpp"), "gate must fire without -DFEATURE").not.toEqual([]);

        database(s, units(["main.cpp"], ["-DFEATURE"]));
        expect(await events(s, "cdb")).toBe(1);
        expect(await s.errors("main.cpp"), "open file must pick up the new flags").toEqual([]);
    },
);

serve.files({ "main.cpp": GATED_MAIN }, cxx17(["main.cpp"]))(
    "cdb stamped tick settles",
    async ({ s }) => {
        const stamped = { force: false };
        expect(await events(s, "cdb", stamped), "unchanged stamp must be quiet").toBe(0);

        database(s, units(["main.cpp"], ["-DFEATURE"]));
        expect(
            await events(s, "cdb", stamped),
            "a fresh stamp only arms the settling debounce",
        ).toBe(0);
        expect(await events(s, "cdb", stamped), "the settled stamp reloads").toBe(1);
    },
);

serve.files({ "main.cpp": "int main() { return 0; }\n" }, cxx17(["main.cpp"]))(
    "cdb new entry indexed",
    async ({ s }) => {
        await s.compiled("main.cpp");

        s.disk.write("lib.cpp", "int lib_entry() { return 1; }\n");
        database(s, units(["main.cpp", "lib.cpp"]));
        expect(await events(s, "cdb")).toBe(1);

        await s.indexed();
        expect(await indexes(s, "lib_entry"), "file added to the CDB was never indexed").toBe(true);
    },
);

serve.files(
    { "header.h": "inline int shared() { return 0; }\n", "gone.cpp": '#include "header.h"\n' },
    cxx17(["gone.cpp"]),
)("cdb removed entry recheck", async ({ s }) => {
    const header = s.uri("header.h");
    let result = await s.client.queryContext(header);
    expect(result.total, "gone.cpp must host the header initially").toBeGreaterThanOrEqual(1);

    database(s, {});
    expect(await events(s, "cdb")).toBe(1);

    result = await s.client.queryContext(header);
    expect(result.total, "removed entry must stop hosting the header").toBe(0);
});

serve.files(
    { "main.cpp": GATED_MAIN, "lib.cpp": "int lib_entry() { return 1; }\n" },
    { manifest: { units: {} } },
)("cdb appears after startup", async ({ s }) => {
    await s.offline(() => {
        s.workspace.rm("compile_commands.json");
    });
    expect(await s.errors("main.cpp"), "guessed command cannot define FEATURE").not.toEqual([]);

    // The editor was opened first; cmake runs later.
    database(s, units(["main.cpp", "lib.cpp"], ["-DFEATURE"]));
    expect(await events(s, "cdb")).toBe(1);

    expect(await s.errors("main.cpp"), "open file must switch to the discovered CDB").toEqual([]);
    await s.indexed();
    expect(
        await indexes(s, "lib_entry"),
        "closed file from the discovered CDB was never indexed",
    ).toBe(true);
});

serve.files(
    {
        "header.h": HEADER_V1,
        "main.cpp":
            '#include "header.h"\nstatic_assert(VALUE == 2, "");\nint main() { return 0; }\n',
        "closed.cpp": CLOSED,
    },
    cxx17(["main.cpp", "closed.cpp"]),
)("checkout updates workspace", async ({ s }) => {
    expect(await s.errors("main.cpp"), "static_assert must fire against header V1").not.toEqual([]);
    await s.indexed();
    expect(
        await referrers(s, ALPHA),
        "initial index never resolved the closed TU's alpha call",
    ).toContain("closed.cpp");

    expect(await events(s, "workspace")).toBe(0);

    // Simulate git checkout: rewrite files on disk, no didSave.
    s.disk.write("header.h", HEADER_V2);
    s.disk.write("closed.cpp", CLOSED + "int checkout_added() { return 3; }\n");
    expect(await events(s, "workspace")).toBe(2);

    expect(await s.errors("main.cpp"), "open file must compile against the new header").toEqual([]);
    await s.indexed();
    expect(
        await referrers(s, BETA),
        "closed TU was not reindexed against the new header",
    ).toContain("closed.cpp");
    expect(await indexes(s, "checkout_added"), "closed TU's own disk change was not indexed").toBe(
        true,
    );
});

serve.files({ "header.h": HEADER_V1, "closed.cpp": CLOSED }, cxx17(["closed.cpp"]))(
    "checkout under an open header",
    async ({ s }) => {
        await s.indexed();
        expect(await referrers(s, ALPHA)).toContain("closed.cpp");
        s.open("header.h");
        expect(await events(s, "workspace")).toBe(0);

        // The editor reloads a clean buffer after a checkout: didChange, no
        // didSave. The buffer shadows the disk for the header's own compile
        // only, so the closed includer sees the checkout while it stays open.
        s.disk.write("header.h", HEADER_V2);
        s.edit("header.h", { text: HEADER_V2 });
        expect(await events(s, "workspace")).toBe(1);
        await s.indexed();
        expect(
            await referrers(s, BETA),
            "closed TU was not reindexed while the header stayed open",
        ).toContain("closed.cpp");
    },
);

serve.files(
    {
        "header.h": HEADER_V1,
        "closed.cpp":
            '#define HEADER "header.h"\n#include HEADER\nint use_target() { return TARGET(); }\n',
    },
    cxx17(["closed.cpp"]),
)("macro include change reindexes", async ({ s }) => {
    await s.indexed();
    expect(await referrers(s, ALPHA)).toContain("closed.cpp");
    expect(await events(s, "workspace")).toBe(0);

    // Only the compile resolves the include: the header is watched and its
    // includer found through what the indexed compile read.
    s.disk.write("header.h", HEADER_V2);
    expect(await events(s, "workspace")).toBe(1);
    await s.indexed();
    expect(await referrers(s, BETA), "the macro includer was not reindexed").toContain(
        "closed.cpp",
    );
});

// Where a failed include looked is watched: creating the header there
// recompiles the open includer and reindexes the closed one.
serve.files(
    {
        "open.cpp": '#include "gen.h"\nint use_a() { return make(); }\n',
        "closed.cpp": '#include "gen.h"\nint use_b() { return make(); }\n',
    },
    cxx17(["open.cpp", "closed.cpp"]),
)("created header reaches includers", async ({ s }) => {
    expect(await s.errors("open.cpp"), "gen.h does not exist yet").not.toEqual([]);
    await s.indexed();
    expect(await indexes(s, "use_b")).toBe(true);

    s.disk.write("gen.h", "int make();\n");
    expect(await events(s, "workspace")).toBe(1);
    expect(await s.errors("open.cpp"), "the open includer must find the new header").toEqual([]);
    await s.indexed();
    expect(
        await referrers(s, at("gen.h", "make")),
        "the closed includer was not reindexed",
    ).toContain("closed.cpp");
});

// The includer's index predates the header: its own run must not take
// that index's word that the includer never enters it.
serve.files(
    { "closed.cpp": '#include "gen.h"\nint use_b() { return make(); }\n' },
    cxx17(["closed.cpp"]),
)("created header indexes in its includer", async ({ s }) => {
    await s.indexed();
    s.disk.write("gen.h", "int make();\n");
    expect(await events(s, "workspace")).toBe(1);
    await s.indexed();
    expect(await referrers(s, at("gen.h", "make"))).toContain("closed.cpp");
});

serve.files(
    {
        "h.h": "#pragma once\nextern int shared_sym;\n",
        "a.cpp": '#include "h.h"\nint use_a() { return shared_sym; }\n',
        "b.cpp": '#include "h.h"\nint use_b() { return shared_sym; }\n',
        "c.cpp": "int shared_sym = 1;\n",
    },
    cxx17(["a.cpp", "b.cpp", "c.cpp"]),
)("dependency change keeps buffer rows", async ({ s }) => {
    await s.compiled("a.cpp");
    await s.indexed();
    expect(await indexes(s, "use_b")).toBe(true);
    await s.compiled("b.cpp");
    s.edit("b.cpp", { after: "return shared_sym; }\n", insert: "// unsaved\n" });
    await s.compiled("b.cpp");
    const bSites = async () =>
        ((await s.references(at("a.cpp", "shared_sym"))) ?? []).filter(
            (site) => s.relative(site.uri) === "b.cpp",
        ).length;
    expect(await bSites()).toBe(1);

    // The header moves on disk: b.cpp's compile is stale, its buffer is
    // not, so the rows it compiled from these very bytes keep serving.
    // Settled first, as in "delete while open reported".
    await s.sync();
    expect(await events(s, "workspace")).toBe(0);
    s.disk.write("h.h", "#pragma once\n// moved\nextern int shared_sym;\n");
    expect(await events(s, "workspace")).toBe(1);
    expect(await bSites(), "an edited buffer's rows vanished on a dependency change").toBe(1);
});

serve.files({ "header.h": HEADER_V1, "main.cpp": '#include "header.h"\n' }, cxx17(["main.cpp"]))(
    "touch emits no events",
    async ({ s }) => {
        expect(await events(s, "workspace")).toBe(0);

        // mtime bump, identical bytes: the content-hash check must stay
        // silent; the wait makes the rewrite's mtime a later one.
        await sleep(MTIME_GRANULARITY);
        s.disk.write("header.h", HEADER_V1);
        expect(await events(s, "workspace")).toBe(0);
    },
);

serve.files(
    { "main.cpp": GATED_MAIN },
    cxx17(["main.cpp"], [], { config: { tracker: { workspace_poll_seconds: 1 } } }),
)("cdb polling loop live", async ({ s }) => {
    expect(await s.errors("main.cpp")).not.toEqual([]);
    await s.indexed();

    // No hook: the polling loop reloads the database on its own, and the
    // reindex the new flags queue is the event waited for.
    const reindex = await s.hold("index", "main.cpp");
    database(s, units(["main.cpp"], ["-DFEATURE"]));
    await reindex.reached();
    await reindex.release();
    expect(await s.errors("main.cpp"), "the polling loop must reload the CDB on its own").toEqual(
        [],
    );
});

serve.files(
    { "main.cpp": "int main() { return 0; }\n", "lib.cpp": GATED_LIB },
    cxx17(["main.cpp", "lib.cpp"]),
)("cdb flag change reindexes closed", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.indexed();
    expect(await indexes(s, "feature_off"), "closed file was never indexed initially").toBe(true);

    // Only lib.cpp's flags change; its bytes do not. Content-based staleness
    // cannot see this — the CDB delta must force the reindex.
    database(s, units(["main.cpp", "lib.cpp"], ["-DFEATURE"]));
    expect(await events(s, "cdb")).toBe(1);

    await s.indexed();
    expect(
        await indexes(s, "feature_on"),
        "closed file was not reindexed after its flags changed",
    ).toBe(true);
});

serve.files({ "header.h": HEADER_V1, "closed.cpp": CLOSED }, cxx17(["closed.cpp"]))(
    "rewrite before first tick reported",
    async ({ s }) => {
        await s.indexed();
        expect(
            await referrers(s, ALPHA),
            "initial index never resolved the closed TU's alpha call",
        ).toContain("closed.cpp");

        // No seeding tick: the first one judges the header against the bytes
        // the startup scan read.
        s.disk.write("header.h", HEADER_V2);
        expect(await events(s, "workspace")).toBe(1);
        await s.indexed();
        expect(
            await referrers(s, BETA),
            "closed TU was not reindexed against the rewritten header",
        ).toContain("closed.cpp");
    },
);

serve.files(
    { "header.h": HEADER_V1, "main.cpp": '#include "header.h"\nint main() { return VALUE; }\n' },
    cxx17(["main.cpp"]),
)("delete while open reported", async ({ s }) => {
    // A buffer shadows the disk for its own file's compile only: the
    // removal is main.cpp's news while the header is still open.
    s.open("header.h");
    // Settled: work still running would look at the header and report its
    // removal before the poll does.
    await s.sync();
    expect(await events(s, "workspace")).toBe(0);
    s.disk.rm("header.h");
    expect(await events(s, "workspace"), "an open file's removal is reported").toBe(1);
    s.close("header.h");
    expect(await events(s, "workspace"), "reported once").toBe(0);
});

serve.files(
    {
        "h.h": "#pragma once\ninline int helper() { return 1; }\n",
        "a.cpp": '#include "h.h"\nint use() { return helper(); }\n',
    },
    cxx17(["a.cpp"], [], { config: { project: { enable_indexing: false } } }),
)("unchanged save no recompile", async ({ s }) => {
    s.open("h.h");
    expect(await s.errors("a.cpp")).toEqual([]);

    // A recompile of the host counts a compile and publishes fresh
    // diagnostics; a hover on a clean AST does neither.
    const host = async () => {
        await s.sync();
        const builds = (await s.counts()).files["a.cpp"];
        return { compile: builds?.compile, publish: builds?.publish };
    };
    const hover = () => s.hover(at("a.cpp", "helper()"));
    const settled = await host();
    await hover();
    expect(await host(), "control: a clean AST serves the hover").toEqual(settled);
    s.save("h.h");
    await hover();
    expect(await host(), "saving unchanged bytes must not dirty the host").toEqual(settled);
});

serve.files(
    { "main.cpp": "#ifndef NEW\n#error missing NEW\n#endif\nint main() { return 0; }\n" },
    cxx17(["main.cpp"], ["-DOLD"], { config: { project: { enable_indexing: false } } }),
)("same stamp cdb rewrite applied", async ({ s }) => {
    // A stamp the filesystem cannot vouch for: the mtime is not safely in
    // the past, so an unchanged stat proves nothing about the bytes. The
    // server must first read the database with it.
    const cdb = s.workspace.path("compile_commands.json");
    const stamp = new Date(Date.now() + 3_600_000);
    await s.offline(() => {
        fs.utimesSync(cdb, stamp, stamp);
    });
    expect(await s.errors("main.cpp")).toHaveLength(1);

    const stamped = { force: false };
    expect(await events(s, "cdb", stamped)).toBe(0);
    // In place, same length, same mtime: only the content tells.
    const before = fs.statSync(cdb, { bigint: true });
    s.disk.edit("compile_commands.json", { replace: "-DOLD", with: "-DNEW" });
    fs.utimesSync(cdb, stamp, stamp);
    const after = fs.statSync(cdb, { bigint: true });
    expect(after.size).toBe(before.size);
    expect(after.mtimeNs).toBe(before.mtimeNs);

    // The new content settles like any rewrite: seen on two polls.
    expect(await events(s, "cdb", stamped)).toBe(0);
    expect(await events(s, "cdb", stamped)).toBe(1);
    expect(await s.errors("main.cpp"), "the rewritten flag must reach the open file").toEqual([]);
});

/// Flags giving the TU a sysroot inside the workspace: the driver adds its
/// include directories itself, so the headers there count as installed
/// ones, like a toolchain's.
function sysrootArgs(s: Serve): string[] {
    return ["--target=x86_64-unknown-linux-gnu", `--sysroot=${s.workspace.path("sysroot")}`];
}

serve.files(
    {
        "sysroot/usr/include/installed.h": "#define INSTALLED 1\n",
        "local.h": "#define LOCAL 1\n",
        "main.cpp":
            '#include <installed.h>\n#include "local.h"\nint main() { return INSTALLED + LOCAL; }\n',
    },
    // clang-tidy's configuration lookups would add their own looks.
    cxx17([], [], {
        config: { project: { enable_indexing: false }, diagnostics: { clang_tidy: false } },
    }),
)("requests look at workspace files only", async ({ s }) => {
    await offlineDatabase(s, { "main.cpp": sysrootArgs(s) });
    expect(await s.errors("main.cpp")).toEqual([]);
    const hover = () => s.hover(at("main.cpp", "main()"));
    await hover();

    const before = await s.client.stats();
    await hover();
    const after = await s.client.stats();
    expect(after.checksLooked - before.checksLooked, "the workspace header is looked at").toBe(1);
    expect(after.checksTrusted - before.checksTrusted, "the installed header is not").toBe(1);
});

// Finding the PCH stale, the request rebuilds it: the check, the PCH's
// preparation and its build share one look at each header.
serve.files(
    {
        "a.h": "#pragma once\ninline int a() { return 1; }\n",
        "b.h": "#pragma once\ninline int b() { return 2; }\n",
        "main.cpp": '#include "a.h"\n#include "b.h"\nint main() { return a() + b(); }\n',
    },
    cxx17(["main.cpp"], [], { config: { project: { enable_indexing: false } } }),
)("stale request looks once", async ({ s }) => {
    await s.compiled("main.cpp");

    const looked = async (request: () => Promise<unknown>) => {
        const before = await s.client.stats();
        await request();
        return (await s.client.stats()).checksLooked - before.checksLooked;
    };
    const hover = () => s.hover(at("main.cpp", "main()"));
    const completion = () => s.request("textDocument/completion", at("main.cpp", "return |a()"));
    const freshHover = await looked(hover);
    const freshCompletion = await looked(completion);

    s.disk.write("b.h", "#pragma once\ninline int b() { return 22; }\n");
    expect(await looked(hover)).toBe(freshHover);
    s.disk.write("a.h", "#pragma once\ninline int a() { return 11; }\n");
    expect(await looked(completion)).toBe(freshCompletion);
});

serve.files(
    {
        "sysroot/usr/include/installed.h": "#define INSTALLED 1\n",
        "main.cpp": '#include <installed.h>\nstatic_assert(INSTALLED == 2, "");\n',
    },
    cxx17([], [], { config: { project: { enable_indexing: false } } }),
)("save looks at installed headers", async ({ s }) => {
    await offlineDatabase(s, { "main.cpp": sysrootArgs(s) });
    expect(await s.errors("main.cpp"), "the installed header defines 1").not.toEqual([]);

    // An upgrade rewrites the installed header; nothing asks until a save.
    // Of the same size, the rewrite is told apart by a later mtime.
    await sleep(MTIME_GRANULARITY);
    s.disk.write("sysroot/usr/include/installed.h", "#define INSTALLED 2\n");
    s.save("main.cpp");
    expect(await s.errors("main.cpp"), "the save must look at the installed header").toEqual([]);
});

serve.files(
    { "header.h": HEADER_V1, "closed.cpp": CLOSED },
    cxx17(["closed.cpp"], [], { config: { tracker: { workspace_poll_seconds: 1 } } }),
)("background ticks see a rewrite", async ({ s }) => {
    await s.indexed();
    expect(await referrers(s, ALPHA)).toContain("closed.cpp");

    // No hook, no save, and the index answers without looking at the disk:
    // only a tick can see the rewrite, and the reindex it queues is the
    // event waited for.
    const reindex = await s.hold("index", "closed.cpp");
    s.disk.write("header.h", HEADER_V2);
    await reindex.reached();
    await reindex.release();
    await s.indexed();
    expect(await referrers(s, BETA), "a background tick must see the rewrite").toContain(
        "closed.cpp",
    );
});
