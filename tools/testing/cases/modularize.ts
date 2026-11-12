/// The build the plan of a `clice modularize` run describes, compiled as a
/// case checks it: that build's database in place of the one the run read,
/// and a server asked for every unit's errors.

import type { CommandRun } from "../cli/run.ts";
import type { TestContext } from "./context.ts";
import type { Entry } from "./project.ts";

/// What the build reads of the plan `clice modularize` and `clice
/// modularize merge` print (docs/en/cli/modularize.md, "Output").
export interface Plan {
    wrapping: {
        stdSources: string[];
        modules: { name: string; source: string; includeRoots: string[]; mirrors: string[] }[];
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

export interface BuildOptions {
    /// Arguments every unit takes: for a merged build, the defines under
    /// which one configuration's condition holds.
    args?: readonly string[];
    /// For a merged build, the configuration's own run, whose plan gives
    /// what the merge's leaves out: the standard library's module sources
    /// and each module's include roots.
    configuration?: CommandRun;
}

/// The database of the build `plan` describes, its outputs under `out`,
/// over `read`, the database the run read: the standard library's module
/// sources, the wrapped modules with their mirrors and include roots first
/// on their include path, the rewritten modules' units, and the rest of the
/// program, but a file gone (`exists`), with the mirrors first and the
/// prelude forced in, but where the rewrite includes it. The project root,
/// where the prelude is, is on the include path of every unit but a module
/// the plan wraps. A new unit takes the compiler and the flags of the
/// first entry read; every unit takes `args`. `own` is the plan of the
/// configuration's own run, for a merged build (`BuildOptions`).
export function planned(
    read: readonly Entry[],
    plan: Plan,
    out: string,
    exists: (file: string) => boolean,
    { args = [], own = plan }: { args?: readonly string[]; own?: Plan } = {},
): Entry[] {
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
        arguments: [compiler, ...args, ...extra, ...flags, `${directory}/${file}`],
    });
    const mirrors = (dirs: string[]) => dirs.map((dir) => `-I${out}/${dir}`);
    const roots = (name: string): string[] => {
        const module = own.wrapping.modules.find((candidate) => candidate.name === name);
        if (module === undefined) {
            throw new Error(`the configuration's plan wraps no module ${name}`);
        }
        return module.includeRoots;
    };
    const rewritten = plan.rewriting.modules.flatMap((module) => [
        module.primary,
        ...module.interfaces,
        ...module.partitions,
        ...module.sources,
    ]);
    return [
        ...own.wrapping.stdSources.map((source) => unit(source, [])),
        ...plan.wrapping.modules.map((module) =>
            unit(`${out}/${module.source}`, [
                ...mirrors(module.mirrors),
                ...roots(module.name).map((root) => `-I${root}`),
            ]),
        ),
        ...rewritten.map((file) => unit(file, ["-I."])),
        ...read
            .filter((entry) => !rewritten.includes(relative(entry)) && exists(relative(entry)))
            .map((entry) => ({
                ...entry,
                arguments: [
                    compiler,
                    ...args,
                    ...mirrors(plan.wrapping.mirrors),
                    "-I.",
                    ...(plan.rewriting.importers.includes(relative(entry))
                        ? []
                        : ["-include", `${out}/${plan.wrapping.prelude}`]),
                    ...entry.arguments.slice(1),
                ],
            })),
    ];
}

/// Fail unless the program compiles cleanly as the plan `run` printed
/// builds it, its outputs under `out`: the build's database replaces the
/// copy's for a server that asks every unit's errors, then stops, and the
/// copy's comes back.
export async function compiles(
    t: TestContext,
    run: CommandRun,
    out: string,
    { args = [], configuration = run }: BuildOptions = {},
): Promise<void> {
    const read = t.disk.read("compile_commands.json");
    const database = planned(
        JSON.parse(read) as Entry[],
        run.json as Plan,
        out,
        (file) => t.disk.list(file).length === 1,
        { args, own: configuration.json as Plan },
    );
    t.disk.write("compile_commands.json", `${JSON.stringify(database, null, 2)}\n`);
    const s = await t.start();
    const errors: string[] = [];
    for (const { directory, file } of database) {
        const unit = file.slice(directory.length + 1);
        errors.push(...(await s.errors(unit)).map((error) => `${unit}: ${error.message}`));
    }
    await s.stop();
    t.disk.write("compile_commands.json", read);
    if (errors.length > 0) {
        throw new Error(`the build the plan describes does not compile:\n  ${errors.join("\n  ")}`);
    }
}
