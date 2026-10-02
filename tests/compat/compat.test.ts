/// Real build systems and real toolchains: each scenario builds the
/// shared project, then clice must parse every unit of the database the
/// build wrote as cleanly as the compiler did, agree with the compiler on
/// the macros its flags imply, and resolve each file's command as the
/// scenario expects.

import * as fs from "node:fs";
import * as path from "node:path";
import { compileCommand, containsRun, lint } from "@clice/tools/compat/clice";
import {
    compilerMacros,
    entrySource,
    writeExpectations,
    type DatabaseEntry,
} from "@clice/tools/compat/macros";
import { build, materialize, missingTools } from "@clice/tools/compat/scenario";
import { cliceExecutable } from "@clice/tools/session";
import { expect, onTestFinished, test } from "vitest";
import { SCENARIOS } from "./scenarios.ts";

function readDatabase(root: string): DatabaseEntry[] {
    const found = ["compile_commands.json", "build/compile_commands.json"]
        .map((rel) => path.join(root, rel))
        .filter((file) => fs.existsSync(file));
    expect(found, "the build wrote one compilation database").toHaveLength(1);
    return JSON.parse(fs.readFileSync(found[0] ?? "", "utf8")) as DatabaseEntry[];
}

for (const scenario of SCENARIOS.filter((s) => s.platforms.includes(process.platform))) {
    const missing = missingTools(scenario);
    // CI installs every tool, so a missing one there is a broken setup
    // that must fail rather than quietly shrink the matrix.
    test.skipIf(missing.length > 0 && process.env["CI"] === undefined)(
        `${scenario.name} on ${process.platform}`,
        () => {
            expect(missing, "tools the scenario runs").toEqual([]);
            const clice = cliceExecutable();
            const root = materialize();
            onTestFinished(() => {
                fs.rmSync(root, { recursive: true, force: true });
            });

            build(scenario, root);
            const entries = readDatabase(root);
            const scratch = path.join(root, ".compat");
            fs.mkdirSync(scratch);
            for (const file of Object.keys(scenario.files)) {
                const source = path.join(root, file);
                const entry = entries.find((e) => entrySource(e) === source);
                expect(entry, `${file} has a database entry`).toBeDefined();
                writeExpectations(source, compilerMacros(entry!, scratch));
            }

            const run = lint(clice, root);
            expect(run.report, "every unit parses clean").toMatch(/: 0 findings\.\n$/);
            expect(run.status, run.log.split("\n").slice(-30).join("\n")).toBe(0);

            const substitute = (arg: string) => arg.replaceAll("${root}", root);
            for (const [file, expectation] of Object.entries(scenario.files)) {
                const command = compileCommand(clice, root, file);
                expect(command.toolchain_error, `${file}: the compiler query`).toBeUndefined();
                expect(command.source, file).toBe("database");
                for (const run of expectation.contains ?? []) {
                    expect(
                        containsRun(command.arguments, run.map(substitute)),
                        `${file}: ${run.join(" ")} in ${command.arguments.join(" ")}`,
                    ).toBe(true);
                }
                for (const arg of expectation.excludes ?? []) {
                    expect(command.arguments, file).not.toContain(substitute(arg));
                }
            }
        },
    );
}
