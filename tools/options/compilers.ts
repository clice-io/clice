/// Checks the options src/command/option_classes.inc leaves out against the
/// compilers clice asks about a command but does not compile with: GCC, whose
/// predefined macros and include search a GCC command's query takes, and cl,
/// whose predefined macros clice gives a cl command (flags_for_clang). For
/// each left-out option the compiler spells (GCC: the driver's `-` options;
/// cl: its `/` ones), with each sample value, in C and C++: the compiler's
/// answer with the option against without it. A compiler rejecting the
/// option or ignoring it shows no change; a change is a finding: an option
/// the table must keep.
///
///   node tools/options/compilers.ts [--include <dir>] [--gcc <gcc>]...
///       [--cl <cl.exe>] [--out <file>]
///
/// --include is the release's libclang include directory, as for
/// tools/options/options.ts. The Markdown report goes to stdout, or to --out.
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { parseArgs } from "node:util";
import { runProcess } from "../client/process.ts";
import { REPO_ROOT } from "../repo.ts";
import {
    driverOption,
    fail,
    limiter,
    type Option,
    optionClasses,
    parseClassTable,
    parseOptions,
    read,
    releaseTools,
    variants,
} from "./options.ts";

/// What a compiler answers for a command: its predefined macros by name,
/// and its include search, in order.
export interface Answer {
    macros: Map<string, string>;
    search: string[];
}

/// The macros a `-dM` or `/PD` dump defines, by name.
export function parseMacros(stdout: string): Map<string, string> {
    const macros = new Map<string, string>();
    for (const line of stdout.split(/\r?\n/)) {
        const match = /^#define (\S+) ?(.*)$/.exec(line);
        if (match?.[1] !== undefined) {
            macros.set(match[1], match[2] ?? "");
        }
    }
    return macros;
}

/// The directories GCC's `-v` lists between `#include "..." search starts
/// here:` and `End of search list.`.
export function searchList(stderr: string): string[] {
    const lines = stderr.split(/\r?\n/);
    const start = lines.findIndex((line) => line.startsWith("#include "));
    const end = lines.findIndex((line) => line.startsWith("End of search list"));
    return start < 0 || end < start
        ? []
        : lines
              .slice(start + 1, end)
              .filter((line) => !line.startsWith("#include "))
              .map((line) => path.normalize(line.trim()));
}

/// How an answer differs from the one without the option: the macros it
/// adds or changes (`+NAME=VALUE`) and removes (`-NAME`), the directories it
/// adds (`+dir`) and removes (`-dir`).
export function changes(before: Answer, now: Answer): string[] {
    return [
        ...[...now.macros]
            .filter(([name, value]) => before.macros.get(name) !== value)
            .map(([name, value]) => `+${name}=${value}`),
        ...[...before.macros.keys()]
            .filter((name) => !now.macros.has(name))
            .map((name) => `-${name}`),
        ...now.search.filter((dir) => !before.search.includes(dir)).map((dir) => `+${dir}`),
        ...before.search.filter((dir) => !now.search.includes(dir)).map((dir) => `-${dir}`),
    ];
}

/// A compiler run that takes longer hangs.
const RUN_TIMEOUT = 30_000;

interface Compiler {
    name: string;
    /// The left-out options it spells.
    spells: (option: Option) => boolean;
    /// Its answer for `variant` in `language` ("c" or "c++"), none where it
    /// rejects the command or dumps no macros.
    answer: (variant: readonly string[], language: string) => Promise<Answer | undefined>;
}

const run = limiter(8);

function gcc(program: string, scratch: string): Compiler {
    return {
        name: path.basename(program),
        spells: (option) =>
            option.visibility.includes("DefaultVis") && !option.spelling.startsWith("/"),
        answer: async (variant, language) => {
            const [macros, search] = await Promise.all(
                [
                    ["-E", "-dM", "-x", language, "-"],
                    ["-E", "-v", "-x", language, "-o", os.devNull, "-"],
                ].map((query) =>
                    run(() =>
                        runProcess(program, [...variant, ...query], {
                            cwd: scratch,
                            input: "",
                            timeout: RUN_TIMEOUT,
                        }),
                    ),
                ),
            );
            if (macros?.status !== 0 || search?.status !== 0) {
                return undefined;
            }
            const defined = parseMacros(macros.stdout);
            return defined.size === 0
                ? undefined
                : { macros: defined, search: searchList(search.stderr) };
        },
    };
}

function cl(program: string, scratch: string): Compiler {
    for (const name of ["empty.c", "empty.cpp"]) {
        fs.writeFileSync(path.join(scratch, name), "");
    }
    return {
        name: path.basename(program),
        spells: (option) =>
            option.visibility.includes("CLOption") && option.spelling.startsWith("/"),
        answer: async (variant, language) => {
            const file = path.join(scratch, language === "c" ? "empty.c" : "empty.cpp");
            const ran = await run(() =>
                runProcess(
                    program,
                    ["/nologo", "/Zc:preprocessor", "/PD", "/EP", ...variant, file],
                    {
                        cwd: scratch,
                        timeout: RUN_TIMEOUT,
                    },
                ),
            );
            const defined = ran.status === 0 ? parseMacros(ran.stdout) : new Map<string, string>();
            return defined.size === 0 ? undefined : { macros: defined, search: [] };
        },
    };
}

export interface Finding {
    option: Option;
    compiler: string;
    language: string;
    variant: readonly string[];
    changes: string[];
}

/// The changes clice does not follow by design, by option, with the reason
/// a report gives.
export const ACCEPTED: ReadonlyMap<string, string> = new Map([
    [
        "_SLASH_Zc___cplusplus",
        "clang-cl's __cplusplus follows the standard whatever /Zc:__cplusplus says, and a cl command gets clang-cl's",
    ],
]);

/// Each option's findings, one line per sample, the compilers sharing a
/// change on one line.
function optionLines(findings: readonly Finding[]): string[] {
    const groups = new Map<string, Finding[]>();
    for (const finding of findings) {
        groups.set(finding.option.id, [...(groups.get(finding.option.id) ?? []), finding]);
    }
    const lines: string[] = [];
    for (const group of [...groups.values()].sort((a, b) =>
        (a[0]?.option.spelling ?? "").localeCompare(b[0]?.option.spelling ?? ""),
    )) {
        const first = group[0];
        if (first === undefined) {
            continue;
        }
        const reason = ACCEPTED.get(first.option.id);
        lines.push(
            `- \`${first.option.spelling}\` ${first.option.id}` +
                (reason === undefined ? "" : `: ${reason}`),
        );
        const shown = new Map<string, Finding>();
        for (const finding of group) {
            const key = `${finding.variant.join(" ")} ${finding.changes.join(" ")}`;
            const where = `${finding.compiler} ${finding.language}`;
            shown.set(key, {
                ...finding,
                compiler: shown.has(key) ? `${shown.get(key)?.compiler ?? ""}, ${where}` : where,
            });
        }
        for (const finding of [...shown.values()].sort((a, b) =>
            a.variant.join(" ").localeCompare(b.variant.join(" ")),
        )) {
            lines.push(
                `  - \`${finding.variant.join(" ")}\` (${finding.compiler}): ${finding.changes.slice(0, 8).join(", ")}`,
            );
        }
    }
    return lines;
}

/// The report: the options the table must keep, then the changes accepted.
export function render(header: readonly string[], findings: readonly Finding[]): string {
    const fresh = optionLines(findings.filter((finding) => !ACCEPTED.has(finding.option.id)));
    const lines = [
        ...header,
        "",
        "## Options the table must keep",
        "",
        ...(fresh.length === 0 ? ["None."] : fresh),
        "",
        "## Accepted",
        "",
        ...optionLines(findings.filter((finding) => ACCEPTED.has(finding.option.id))),
    ];
    return `${lines.join("\n")}\n`;
}

const USAGE =
    "usage: node tools/options/compilers.ts [--include <dir>] [--gcc <gcc>]... [--cl <cl.exe>] [--out <file>]";

async function main(): Promise<void> {
    let values;
    try {
        ({ values } = parseArgs({
            options: {
                include: { type: "string" },
                gcc: { type: "string", multiple: true },
                cl: { type: "string" },
                out: { type: "string" },
                help: { type: "boolean", short: "h" },
            },
        }));
    } catch (error) {
        fail(`${USAGE}\nerror: ${error instanceof Error ? error.message : String(error)}`);
    }
    if (values.help === true) {
        console.log(USAGE);
        return;
    }
    const { include } =
        values.include === undefined
            ? releaseTools(undefined, undefined)
            : { include: values.include };
    const options = parseOptions(read(path.join(include, "clang", "Options", "Options.inc")));
    const classes = optionClasses(
        options,
        parseClassTable(read(path.join(REPO_ROOT, "src", "command", "option_classes.inc"))),
    );
    const scratch = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "clice-compilers-")));
    try {
        const compilers = [
            ...(values.gcc ?? []).map((program) => gcc(program, scratch)),
            ...(values.cl === undefined ? [] : [cl(values.cl, scratch)]),
        ];
        if (compilers.length === 0) {
            fail(`${USAGE}\nerror: name a --gcc or a --cl`);
        }
        const leftOut = [...options.values()].filter(
            (option) => driverOption(option) && classes.get(option.id) === "LeftOut",
        );
        const file = path.join(scratch, "f.txt");
        fs.writeFileSync(file, "");
        const findings: Finding[] = [];
        for (const compiler of compilers) {
            for (const language of ["c", "c++"]) {
                const base = await compiler.answer([], language);
                if (base === undefined) {
                    fail(`${compiler.name} gives no ${language} answer of its own`);
                }
                await Promise.all(
                    leftOut.filter(compiler.spells).map(async (option) => {
                        for (const variant of variants(option, file)) {
                            const now = await compiler.answer(variant, language);
                            const changed = now === undefined ? [] : changes(base, now);
                            if (changed.length > 0) {
                                findings.push({
                                    option,
                                    compiler: compiler.name,
                                    language,
                                    variant,
                                    changes: changed,
                                });
                            }
                        }
                    }),
                );
            }
        }
        const report = render(
            [
                "# The options the table leaves out, against the compilers clice queries",
                "",
                `${compilers.map((compiler) => compiler.name).join(", ")}: each left-out option it spells, ` +
                    "with its sample values, in C and C++.",
            ],
            findings,
        );
        if (values.out === undefined) {
            process.stdout.write(report);
        } else {
            fs.writeFileSync(values.out, report);
        }
    } finally {
        fs.rmSync(scratch, { recursive: true, force: true });
    }
}

if (import.meta.main) {
    main().catch((error: unknown) => {
        fail(error instanceof Error ? error.message : String(error));
    });
}
