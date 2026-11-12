import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

interface Plan {
    wrapping: {
        stdSources: string[];
        modules: { source: string; includeRoots: string[]; mirrors: string[] }[];
        mirrors: string[];
        prelude: string;
    };
    rewriting: {
        modules: {
            primary: string;
            interfaces: string[];
            partitions: string[];
            sources: string[];
        }[];
        importers: string[];
    };
}

interface Entry {
    directory: string;
    file: string;
    arguments: string[];
}

/// Write the database of the build a run's plan describes over the one the
/// run read, and return its units: the wrapped modules with their mirrors
/// and include roots first on their include path, the rewritten modules'
/// units, and the rest of the program with the mirrors first and the
/// prelude forced in, but where the rewrite includes it. The project root,
/// where the prelude is, is on the include path of every unit but a module
/// the plan wraps.
function planned(t: TestContext, plan: Plan, out: string): string[] {
    const read = JSON.parse(t.disk.read("compile_commands.json")) as Entry[];
    const [first] = read;
    if (first === undefined) {
        throw new Error("the run read no database entry");
    }
    const { directory } = first;
    const relative = (entry: Entry): string => entry.file.slice(directory.length + 1);
    const [compiler = "clang++", ...flags] = first.arguments.slice(0, -1);
    const unit = (file: string, extra: string[]): Entry => ({
        directory,
        file: `${directory}/${file}`,
        arguments: [compiler, ...extra, ...flags, `${directory}/${file}`],
    });
    const mirrors = (dirs: string[]) => dirs.map((dir) => `-I${out}/${dir}`);
    const rewritten = plan.rewriting.modules.flatMap((module) => [
        module.primary,
        ...module.interfaces,
        ...module.partitions,
        ...module.sources,
    ]);
    const database = [
        ...plan.wrapping.stdSources.map((source) => unit(source, [])),
        ...plan.wrapping.modules.map((module) =>
            unit(`${out}/${module.source}`, [
                ...mirrors(module.mirrors),
                ...module.includeRoots.map((root) => `-I${root}`),
            ]),
        ),
        ...rewritten.map((file) => unit(file, ["-I."])),
        ...read
            .filter((entry) => !rewritten.includes(relative(entry)))
            .filter((entry) => t.disk.list(relative(entry)).length === 1)
            .map((entry) => ({
                ...entry,
                arguments: [
                    compiler,
                    ...mirrors(plan.wrapping.mirrors),
                    "-I.",
                    ...(plan.rewriting.importers.includes(relative(entry))
                        ? []
                        : ["-include", `${out}/${plan.wrapping.prelude}`]),
                    ...entry.arguments.slice(1),
                ],
            })),
    ];
    t.disk.write("compile_commands.json", `${JSON.stringify(database, null, 2)}\n`);
    return database.map(relative);
}

// core's text.h forward-declares sink.h's Sink and declares hook(), which
// tool's hook.cpp defines, and includes cfg's config.h, which stays a
// header; detail.h is core's alone, all.h an umbrella over it, sink.h
// guarded. tool.h forward-declares core's Sink and Box, and tool.cpp
// expands core's CORE_TWICE. app/run.cpp, outside both, uses tool.
const ARGS = "--scope app/** --scope third/** --std stdmod --out wrap";

// One worker: which header provides fake_widen, declared both in
// <cwide.h> and in a fragment widen.h pastes, depends on the order the
// units are indexed in (analyze/dependencies/06_unit_order).
test("rewrites program modules", async (t) => {
    await t.cli("index --jobs 1");
    const run = await t.cli(`modularize ${ARGS} --partition partitions/program.json`);
    // The program as the plan builds it compiles cleanly.
    const units = planned(t, run.json as Plan, "wrap");
    const s = await t.start();
    for (const unit of units) {
        expect(await s.errors(unit), unit).toEqual([]);
    }
});

// Nothing is written when a partition would be rejected.
test("rejected partitions", async (t) => {
    await t.cli("index --jobs 1");
    for (const partition of ["clash", "same_primary", "outside", "dash", "keyword", "core_only"]) {
        await t.cli(`modularize ${ARGS} --partition partitions/${partition}.json`);
    }
    t.disk.write("app/core/sink.cppm", "");
    await t.cli(`modularize ${ARGS} --partition partitions/both.json`);
    expect(t.disk.read("app/core/sink.cppm")).toBe("");
});
