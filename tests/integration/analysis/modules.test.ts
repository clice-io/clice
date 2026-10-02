/// `clice analyze modules` reads the persisted index and describes how
/// the directories of a workspace depend on each other, as the facts an
/// agent partitions a program into C++20 modules with.

import { spawnSync } from "node:child_process";
import { type Workspace } from "@clice/tools/workspace";
import { cliceExecutable, expect, test } from "../fixtures.ts";

interface Overview {
    modules: { name: string; headers: number; sources: number; layer: number }[];
    edges: {
        from: string;
        to: string;
        interfaceEntities: number;
        implementationEntities: number;
        sample: string[];
    }[];
    cycles: { modules: string[]; cut: { from: string; to: string; sample: string[] }[] }[];
    partitionCycles: { module: string; headers: string[] }[];
    moves: { path: string; from: string; to: string }[];
    internalHeaders: number;
}

interface Obstacles {
    variantHeaders: { path: string; variants: number }[];
    contextMacros: { macro: string; file: string; inCondition: boolean }[];
    internalUses: {
        entity: string;
        owner: string;
        reason: string;
        users: string[];
        exposedInOwner: boolean;
    }[];
    crossModuleRedeclarations: { entity: string; owner: string; file: string; unused: boolean }[];
    foreignDeclarations: { name: string; file: string }[];
    duplicateDefinitions: { entity: string; files: string[] }[];
    configuringMacros: { macro: string; definition: string; readers: string[] }[];
    implicitProviders: { path: string; specializes: string[] }[];
}

const SCOPE = "core/**,util/**,feature/**,meta/**,lib/**,app/**";

function lines(...text: string[]): string {
    return [...text, ""].join("\n");
}

function writeProject(session: { tmpdir(): Workspace }): Workspace {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(
        "core/str.h",
        lines(
            "#pragma once",
            "struct Str { int n; };",
            "struct Third;",
            "int length(const Str& s);",
            "static int helper() { return 1; }",
            "inline int uses_helper() { return helper(); }",
        ),
    );
    ws.write(
        "core/str.cpp",
        lines(
            '#include "core/str.h"',
            '#include "core/a.h"',
            '#include "core/detail.h"',
            '#include "core/mode.h"',
            '#include "core/sink.h"',
            '#include "feature/f.h"',
            '#include "util/dup.h"',
            '#include "util/strops.h"',
            "enum Names {",
            "#define ENTRY(x) x,",
            '#include "core/table.inc"',
            "#undef ENTRY",
            "};",
            "int length(const Str& s) {",
            "    return s.n + helper() + twice(s) + feature_hook(nullptr) + detail_value() +",
            "           Sink{2}.fd;",
            "}",
        ),
    );
    ws.write("core/detail.h", lines("#pragma once", "inline int detail_value() { return 3; }"));
    ws.write("core/sink.h", lines("#pragma once", "struct Sink { int fd; };"));
    ws.write("core/table.inc", "ENTRY(alpha)\nENTRY(beta)\n");
    ws.write(
        "core/a.h",
        lines(
            "#pragma once",
            "struct A { int v; };",
            "struct B;",
            '#include "core/b.h"',
            "inline int a_size() { return sizeof(B); }",
        ),
    );
    ws.write("core/b.h", lines("#pragma once", '#include "core/a.h"', "struct B { A a; };"));
    ws.write(
        "core/log.h",
        lines(
            "#pragma once",
            '#include "util/clock.h"',
            "struct Sink;",
            "struct Logger { Clock clock; Sink* sink; };",
        ),
    );
    ws.write(
        "core/mode.h",
        lines(
            "#pragma once",
            "#ifdef MODE_A",
            "struct ModeA {};",
            "#else",
            "struct ModeB {};",
            "#endif",
        ),
    );
    ws.write("core/config.h", lines("#pragma once", "#define CORE_FAST 1", "#define THIRD_WIDE 1"));
    ws.write(
        "core/traits.h",
        lines(
            "#pragma once",
            '#include "core/str.h"',
            '#include "third/lib.h"',
            "template <> struct Traits<Str> { static constexpr int value = 1; };",
        ),
    );
    ws.write(
        "core/key.h",
        lines(
            "#pragma once",
            '#include "third/lib.h"',
            "struct Key { int k; };",
            "template <> struct Traits<Key> { static constexpr int value = 2; };",
        ),
    );
    ws.write(
        "core/fast.h",
        lines("#pragma once", "#ifdef CORE_FAST", "inline int fast() { return 1; }", "#endif"),
    );
    ws.write(
        "util/clock.h",
        lines(
            "#pragma once",
            '#include "core/str.h"',
            "struct Clock { Str name; int size() const { return length(name); } };",
        ),
    );
    ws.write(
        "util/strops.h",
        lines(
            "#pragma once",
            '#include "core/str.h"',
            "inline int twice(const Str& s) { return length(s) * 2; }",
        ),
    );
    ws.write(
        "util/fmt.h",
        lines("#pragma once", '#include "core/str.h"', "int format_str(const Str& s);"),
    );
    ws.write(
        "util/fmt.cpp",
        lines(
            '#include "util/fmt.h"',
            '#include "util/other.h"',
            "int format_str(const Str& s) { return length(s); }",
            "int spare() { return 0; }",
        ),
    );
    ws.write("util/other.h", lines("#pragma once", "int spare();"));
    ws.write("lib/base.h", lines("#pragma once", "struct Base { int b; };"));
    ws.write(
        "app/detail.h",
        lines(
            "#pragma once",
            '#include "lib/base.h"',
            "inline int base_size() { return sizeof(Base); }",
        ),
    );
    ws.write("util/dup.h", lines("#pragma once", "struct Dup { int x; };"));
    ws.write("feature/dup.h", lines("#pragma once", "struct Dup { int x; };"));
    ws.write(
        "feature/f.h",
        lines(
            "#pragma once",
            "struct Logger;",
            "struct Clock;",
            "int feature_hook(Logger* logger);",
        ),
    );
    ws.write(
        "feature/f.cpp",
        lines(
            '#include "feature/f.h"',
            '#include "feature/dup.h"',
            '#include "core/log.h"',
            "int feature_hook(Logger* logger) { Dup dup{}; return logger ? dup.x : 0; }",
        ),
    );
    ws.write("feature/hook.inc", "int hooked() { Clock clock; return clock.size(); }\n");
    ws.write(
        "meta/m.h",
        lines(
            "#pragma once",
            '#include "util/clock.h"',
            "#define MAKE_CLOCK() Clock{}",
            "inline int meta_size() { return MAKE_CLOCK().size(); }",
        ),
    );
    ws.write(
        "third/lib.h",
        lines(
            "#pragma once",
            "struct Third { int v; };",
            "#ifdef THIRD_WIDE",
            "using ThirdWord = long;",
            "#endif",
            "template <typename T> struct Traits { static constexpr int value = 0; };",
        ),
    );
    ws.write(
        "app/main.cpp",
        lines(
            '#include "core/config.h"',
            '#include "core/fast.h"',
            '#include "core/log.h"',
            '#include "core/mode.h"',
            '#include "meta/m.h"',
            '#include "third/lib.h"',
            '#include "core/traits.h"',
            '#include "core/key.h"',
            '#include "app/detail.h"',
            '#include "util/fmt.h"',
            '#include "feature/hook.inc"',
            "int main() {",
            "    Logger logger; ModeA mode; Third third;",
            "    return logger.clock.size() + helper() + fast() + meta_size() + hooked() +",
            "           format_str(Str{}) + base_size();",
            "}",
        ),
    );
    const include = `-I${ws.root}`;
    ws.writeEntries([
        ["core/str.cpp", [include]],
        ["feature/f.cpp", [include]],
        ["util/fmt.cpp", [include]],
        ["app/main.cpp", [include, "-DMODE_A"]],
    ]);
    return ws;
}

function runClice(...args: string[]) {
    return spawnSync(cliceExecutable(), args, {
        encoding: "utf8",
        timeout: 120_000,
        maxBuffer: 64 * 1024 * 1024,
    });
}

function analyze(ws: Workspace, ...args: string[]): unknown {
    const run = runClice("analyze", "modules", "--workspace", ws.root, "--scope", SCOPE, ...args);
    expect(run.status, `stdout: ${run.stdout}\nstderr: ${run.stderr}`).toBe(0);
    return JSON.parse(run.stdout);
}

function indexed(session: { tmpdir(): Workspace }): Workspace {
    const ws = writeProject(session);
    const run = runClice("index", "--workspace", ws.root, "--workers", "2");
    expect(run.status, `stderr: ${run.stderr}`).toBe(0);
    return ws;
}

function edge(overview: Overview, from: string, to: string) {
    return overview.edges.find((entry) => entry.from === from && entry.to === to);
}

test("directory cycle and its cut", ({ session }) => {
    const ws = indexed(session);
    const overview = analyze(ws) as Overview;

    expect(overview.cycles).toHaveLength(1);
    const [cycle] = overview.cycles;
    expect(cycle?.modules).toEqual(["core", "util"]);
    // util names two of core's entities, core one of util's: the lighter
    // edge is the one to cut.
    expect(cycle?.cut).toHaveLength(1);
    expect(cycle?.cut[0]).toMatchObject({ from: "core", to: "util", sample: ["Clock"] });

    // A source naming an upper module is an implementation edge and closes
    // no cycle.
    expect(edge(overview, "core", "feature")).toMatchObject({ interfaceEntities: 0 });
    expect(edge(overview, "core", "feature")?.implementationEntities).toBeGreaterThan(0);
});

test("macro expansions and fragments", ({ session }) => {
    const ws = indexed(session);
    const overview = analyze(ws) as Overview;

    // Clock appears in meta/m.h only through a macro body.
    expect(edge(overview, "meta", "util")?.sample).toContain("Clock");
    // A fragment's names are charged to the file pasting it in.
    expect(edge(overview, "feature", "util")).toBeUndefined();
    expect(edge(overview, "app", "util")?.sample).toContain("Clock::size");
});

test("partition cycles and moves", ({ session }) => {
    const ws = indexed(session);
    const overview = analyze(ws) as Overview;

    expect(overview.partitionCycles).toEqual([
        { module: "core", headers: ["core/a.h", "core/b.h"] },
    ]);
    expect(overview.moves).toContainEqual({
        path: "util/strops.h",
        from: "util",
        to: "core",
        with: [],
        entities: 3,
        cyclicBefore: 2,
        cyclicAfter: 2,
    });
    // The source implementing a header moves along with it.
    // A source moves with the header it implements, and so does every
    // other header it implements.
    expect(overview.moves).toContainEqual({
        path: "util/fmt.h",
        from: "util",
        to: "app",
        with: ["util/other.h", "util/fmt.cpp"],
        entities: 1,
        cyclicBefore: 2,
        cyclicAfter: 2,
    });
    // Instantiations use a specialization without naming it.
    expect(overview.moves.some((move) => move.path === "core/traits.h")).toBe(false);
});

test("module groups and internal headers", ({ session }) => {
    const ws = indexed(session);
    const detail = analyze(ws, "--view", "module", "--module", "core") as {
        groups: { consumers: string[]; headers: string[] }[];
        internalHeaders: string[];
    };
    // core/log.h names Sink through its own forward declaration.
    expect(detail.internalHeaders).toEqual([
        "core/a.h",
        "core/b.h",
        "core/detail.h",
        "core/sink.h",
    ]);
    const fast = detail.groups.find((group) => group.headers.includes("core/fast.h"));
    expect(fast?.consumers).toEqual(["app"]);
    expect(fast?.headers).toContain("core/mode.h");
    const str = detail.groups.find((group) => group.headers.includes("core/str.h"));
    expect(str?.consumers).toEqual(["app", "util"]);

    // feature/dup.h is internal to feature.
    const overview = analyze(ws) as Overview;
    expect(overview.internalHeaders).toBe(6);

    type Impact = { path: string; partitioned: number; internal: boolean }[];
    const impact = analyze(ws, "--view", "impact") as Impact;
    expect(impact.find((entry) => entry.path === "core/detail.h")).toMatchObject({
        partitioned: 1,
        internal: true,
    });
    // No interface imports lib, but app/main.cpp reaches it through the
    // internal app/detail.h.
    expect(impact.find((entry) => entry.path === "lib/base.h")?.partitioned).toBe(1);
});

test("hypothetical partitions", ({ session }) => {
    const ws = indexed(session);

    const merged = analyze(ws, "--merge", "core+util") as Overview;
    expect(merged.cycles).toHaveLength(0);
    expect(merged.modules.map((module) => module.name).sort()).toEqual([
        "app",
        "core",
        "feature",
        "lib",
        "meta",
    ]);

    const moved = analyze(ws, "--move", "util/strops.h=core") as Overview;
    expect(moved.modules.find((module) => module.name === "util")?.headers).toBe(4);
    expect(moved.moves.some((move) => move.path === "util/strops.h")).toBe(false);

    // With Clock declared in core, core no longer names anything of util.
    const declared = analyze(ws, "--move-entity", "Clock=core/clock.h") as Overview;
    expect(declared.cycles).toHaveLength(0);
    expect(edge(declared, "meta", "core")?.sample).toContain("Clock");

    // What the moved definition names moves with it.
    const carried = analyze(ws, "--move-entity", "Clock=meta/clock.h") as Overview;
    expect(edge(carried, "meta", "core")?.sample).toEqual(
        expect.arrayContaining(["Str", "length"]),
    );
    expect(edge(carried, "core", "meta")?.sample).toContain("Clock");
});

test("edge lists entities and users", ({ session }) => {
    const ws = indexed(session);
    const detail = analyze(ws, "--view", "edge", "--from", "util", "--to", "core") as {
        entities: { id: string; entity: string; users: string[]; interface: boolean }[];
    };
    expect(detail.entities.map((entry) => entry.entity).sort()).toEqual(["Str", "length"]);
    const str = detail.entities.find((entry) => entry.entity === "Str");
    expect(str).toMatchObject({ interface: true });
    expect(str?.id).toMatch(/^#[0-9a-f]{16}$/);
    expect(str?.users).toContain("util/clock.h:3");
});

test("obstacles to the rewrite", ({ session }) => {
    const ws = indexed(session);
    const obstacles = analyze(ws, "--view", "obstacles") as Obstacles;

    expect(obstacles.variantHeaders).toContainEqual(
        expect.objectContaining({
            path: "core/mode.h",
            variants: 2,
            units: 2,
            declarationsDiffer: true,
        }),
    );
    expect(obstacles.contextMacros).toEqual([
        expect.objectContaining({ macro: "CORE_FAST", file: "core/fast.h", inCondition: true }),
    ]);
    expect(obstacles.internalUses).toContainEqual({
        entity: "helper",
        owner: "core/str.h",
        reason: "static",
        users: ["app/main.cpp", "core/str.cpp"],
        exposedInOwner: true,
    });
    expect(obstacles.crossModuleRedeclarations).toContainEqual(
        expect.objectContaining({
            entity: "Logger",
            owner: "core/log.h",
            file: "feature/f.h",
            unused: false,
        }),
    );
    expect(obstacles.crossModuleRedeclarations).toContainEqual(
        expect.objectContaining({ entity: "Clock", file: "feature/f.h", unused: true }),
    );
    expect(obstacles.foreignDeclarations).toContainEqual(
        expect.objectContaining({ name: "Third", file: "core/str.h" }),
    );
    expect(obstacles.duplicateDefinitions).toContainEqual({
        entity: "Dup",
        files: ["feature/dup.h", "util/dup.h"],
    });
    expect(obstacles.configuringMacros).toContainEqual({
        macro: "THIRD_WIDE",
        definition: "core/config.h",
        readers: ["third/lib.h"],
    });
    // core/key.h specializes Traits for its own Key, which its users name.
    expect(obstacles.implicitProviders).toEqual([
        { path: "core/traits.h", specializes: ["Traits"] },
    ]);
});

test("annotations weigh the impact", ({ session }) => {
    const ws = indexed(session);
    ws.write(
        "compile_time.json",
        JSON.stringify({ name: "compile_time", unit: "s", values: { "app/main.cpp": 10 } }),
    );

    type Impact = { path: string; baseline: number; partitioned: number }[];
    const plain = analyze(ws, "--view", "impact") as Impact;
    const weighted = analyze(
        ws,
        "--view",
        "impact",
        "--annotation",
        ws.path("compile_time.json"),
    ) as Impact;
    // core/fast.h is entered by app/main.cpp alone.
    expect(plain.find((entry) => entry.path === "core/fast.h")?.baseline).toBe(1);
    expect(weighted.find((entry) => entry.path === "core/fast.h")?.baseline).toBe(10);
});

test("unknown module fails", ({ session }) => {
    const ws = indexed(session);
    const run = runClice(
        "analyze",
        "modules",
        "--workspace",
        ws.root,
        "--view",
        "module",
        "--module",
        "nowhere",
    );
    expect(run.status).toBe(1);
    expect(JSON.parse(run.stdout)).toEqual({ error: "no module nowhere" });
});
