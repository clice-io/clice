/// Checks that clice compiles a command as clang does, for each option
/// src/command/option_classes.inc keeps: with each sample value, on each
/// target the option tool probes it on, clang's macros and `__has_feature`
/// answers for the command become `#error` checks in a file, which clice
/// compiles (`clice inspect`) from a compile_commands.json naming the same
/// command. A crash, or a check clice fails that it passes without the
/// option, is a finding; the disagreements known and accepted are listed
/// apart with their reasons, so a run that finds nothing new reports none:
///
///   node tools/options/agreement.ts --clice <clice> [--include <dir>]
///       [--clang <clang>] [--only <ID,...>] [--out <file>]
///
/// --include and --clang are the release's, as for tools/options/options.ts;
/// clang++ and clang-cl beside --clang are the compilers the database names.
/// The Markdown report goes to stdout, or to --out.
import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { parseArgs } from "node:util";
import { runProcess } from "../client/process.ts";
import { REPO_ROOT } from "../repo.ts";
import {
    type Compiled,
    driverOption,
    fail,
    groupChain,
    limiter,
    type Option,
    optionClasses,
    parseClassTable,
    parseOptions,
    probeTargets,
    Prober,
    read,
    releaseTools,
    type Target,
    targetArguments,
    unprobed,
    variants,
} from "./options.ts";

/// The disagreements clice keeps by design, by option and, for some, the
/// target, with the reason a report gives.
export const ACCEPTED: readonly { id: string; target?: string; reason: string }[] = [
    {
        id: "fmodules",
        reason: "clice builds no Clang header modules: the command leaves -fmodules out (Normalizer::normalize)",
    },
    ...["fms_compatibility_version", "fmsc_version", "fno_ms_extensions", "m16", "mx32"].map(
        (id) => ({
            id,
            target: "cl",
            reason: "clang-cl's standard comes from its query, which hears the command's /std: alone (ask_cl)",
        }),
    ),
];

export function accepted(id: string, target: string): string | undefined {
    return ACCEPTED.find((entry) => entry.id === id && (entry.target ?? target) === target)?.reason;
}

const INTEGER = /^-?(?:0x[0-9a-fA-F]+|\d+)[uUlL]*$/;

/// The `#error` checks of a compile's macros and feature answers, against
/// those of its target's compile without the option: every macro defined,
/// with its value where it is an integer, none of those the option removes,
/// and the feature answers as they are.
export function checks(now: Compiled, before: Compiled): string {
    const names = (macros: Set<string> | undefined): Map<string, string | undefined> => {
        const found = new Map<string, string | undefined>();
        for (const line of macros ?? []) {
            const match = /^#define (\w+)(\()?\S* ?(.*)$/.exec(line);
            if (match?.[1] !== undefined) {
                found.set(match[1], match[2] === undefined ? match[3] : undefined);
            }
        }
        return found;
    };
    const defined = names(now.macros);
    const lines: string[] = [];
    for (const [name, value] of defined) {
        lines.push(`#ifndef ${name}\n#error "missing ${name}"\n#endif`);
        if (value !== undefined && INTEGER.test(value)) {
            lines.push(`#if (${name}) != (${value})\n#error "value ${name}"\n#endif`);
        }
    }
    for (const name of names(before.macros).keys()) {
        if (!defined.has(name)) {
            lines.push(`#ifdef ${name}\n#error "extra ${name}"\n#endif`);
        }
    }
    for (const feature of now.features ?? []) {
        lines.push(`#if !${feature}\n#error "missing ${feature}"\n#endif`);
    }
    for (const feature of before.features ?? []) {
        if (!now.features?.has(feature)) {
            lines.push(`#if ${feature}\n#error "extra ${feature}"\n#endif`);
        }
    }
    return `${lines.join("\n")}\n`;
}

/// What clice made of a file: the checks it failed, or how it ended.
interface CliceRun {
    crashed?: string;
    failure?: string;
    errors: string[];
}

interface Finding {
    option: Option;
    cls: string;
    target: string;
    variant: readonly string[];
    run: CliceRun;
    accepted: string | undefined;
}

const USAGE =
    "usage: node tools/options/agreement.ts --clice <clice> [--include <dir>] [--clang <clang>] " +
    "[--only <ID,...>] [--out <file>]";

/// A clice run that takes longer hangs.
const CLICE_TIMEOUT = 120_000;

/// clice's compiles of the checks, and the baselines they are read against.
class Checker {
    private readonly run = limiter(6);
    private readonly cliceProgram: string;
    private readonly clang: string;
    private readonly scratch: string;
    private readonly prober: Prober;
    private readonly baselines = new Map<
        string,
        Promise<{ compiled: Compiled; clice: CliceRun }>
    >();
    private projects = 0;

    constructor(clice: string, clang: string, scratch: string, prober: Prober) {
        this.cliceProgram = clice;
        this.clang = clang;
        this.scratch = scratch;
        this.prober = prober;
    }

    /// clice's compile of `source` from a database naming the command.
    async clice(target: Target, variant: readonly string[], source: string): Promise<CliceRun> {
        const dir = path.join(this.scratch, `project-${String(this.projects++)}`);
        fs.mkdirSync(dir);
        fs.writeFileSync(path.join(dir, "probe.cpp"), source);
        const { driver, language } = targetArguments(target);
        const command = [
            path.join(path.dirname(this.clang), target.cl ? "clang-cl" : "clang++"),
            ...driver.filter((arg) => arg !== "--driver-mode=cl"),
            ...language,
            ...variant,
            target.cl ? "/c" : "-c",
            "probe.cpp",
        ];
        fs.writeFileSync(
            path.join(dir, "compile_commands.json"),
            JSON.stringify([{ directory: dir, file: "probe.cpp", arguments: command }]),
        );
        const ran = await this.run(() =>
            runProcess(
                this.cliceProgram,
                ["inspect", "document-symbols", "probe.cpp", `--workspace=${dir}`],
                {
                    cwd: dir,
                    timeout: CLICE_TIMEOUT,
                },
            ),
        );
        fs.rmSync(dir, { recursive: true, force: true });
        const fatal = ran.stderr
            .split(/\r?\n/)
            .find((line) => /LLVM ERROR|crashed|Assertion/.test(line));
        if (ran.signal !== null || fatal !== undefined) {
            return { crashed: [ran.signal, fatal].filter(Boolean).join(": "), errors: [] };
        }
        if (ran.status !== 0) {
            const last = ran.stderr.trim().split(/\r?\n/).at(-1) ?? "";
            return {
                failure: ran.error?.message ?? `exit ${String(ran.status)}: ${last}`,
                errors: [],
            };
        }
        const entry = Object.values(
            (JSON.parse(ran.stdout) as { files: Record<string, { diagnostics?: string[] | null }> })
                .files,
        )[0];
        return { errors: entry?.diagnostics ?? [] };
    }

    baseline(target: Target): Promise<{ compiled: Compiled; clice: CliceRun }> {
        const key = `${target.triple} ${String(target.cl)}`;
        let found = this.baselines.get(key);
        if (found === undefined) {
            found = (async () => {
                const compiled = await this.prober.baseline(target);
                if (typeof compiled === "string") {
                    throw new Error(`clang fails on ${target.triple}: ${compiled}`);
                }
                return {
                    compiled,
                    clice: await this.clice(target, [], checks(compiled, compiled)),
                };
            })();
            this.baselines.set(key, found);
        }
        return found;
    }

    /// The findings of an option on its targets, and the samples clang's own
    /// compile of the checks rejects, which are no evidence.
    async check(
        option: Option,
        cls: string,
        targets: readonly Target[],
        index: number,
    ): Promise<{ findings: Finding[]; unchecked: number }> {
        const directory = path.join(this.scratch, `option-${String(index)}`);
        fs.mkdirSync(directory);
        const file = path.join(directory, "f.txt");
        const findings: Finding[] = [];
        let unchecked = 0;
        for (const target of targets) {
            const base = await this.baseline(target);
            for (const variant of variants(option, file)) {
                fs.writeFileSync(file, "");
                const compiled = await this.prober.compiled(target, variant, directory);
                if (typeof compiled === "string" || !compiled.macros || !compiled.features) {
                    continue;
                }
                const source = checks(compiled, base.compiled);
                const probe = path.join(directory, "probe.cpp");
                fs.writeFileSync(probe, source);
                const { driver, language } = targetArguments(target);
                const own = await this.run(() =>
                    runProcess(
                        this.clang,
                        [
                            ...driver,
                            ...language,
                            ...variant,
                            target.cl ? "/Zs" : "-fsyntax-only",
                            probe,
                        ],
                        { cwd: directory, timeout: CLICE_TIMEOUT },
                    ),
                );
                if (own.status !== 0) {
                    unchecked++;
                    continue;
                }
                const run = await this.clice(target, variant, source);
                const expected = new Set(base.clice.errors);
                const errors = run.errors.filter((error) => !expected.has(error));
                if (run.crashed !== undefined || run.failure !== undefined || errors.length > 0) {
                    findings.push({
                        option,
                        cls,
                        target: target.name,
                        variant,
                        run: { ...run, errors },
                        accepted: accepted(option.id, target.name),
                    });
                }
            }
        }
        return { findings, unchecked };
    }
}

function findingLine(finding: Finding): string {
    const what =
        finding.run.crashed !== undefined
            ? `crashed: ${finding.run.crashed}`
            : (finding.run.failure ?? finding.run.errors.slice(0, 6).join(", "));
    return `  - ${finding.target}, \`${finding.variant.join(" ")}\`: ${what}`;
}

/// The report: the crashes and disagreements first, by option, then those
/// accepted, then what clang's own compile of the checks rejected.
export function render(
    header: readonly string[],
    findings: readonly Finding[],
    unchecked: Map<string, number>,
): string {
    const byOption = (list: readonly Finding[]): string[] => {
        const groups = new Map<string, Finding[]>();
        for (const finding of list) {
            groups.set(finding.option.id, [...(groups.get(finding.option.id) ?? []), finding]);
        }
        return [...groups.values()]
            .sort((a, b) =>
                (a[0]?.option.spelling ?? "").localeCompare(b[0]?.option.spelling ?? ""),
            )
            .flatMap((group) => {
                const first = group[0];
                if (first === undefined) {
                    return [];
                }
                return [
                    `- \`${first.option.spelling}\` ${first.option.id}: ${first.cls}` +
                        (first.accepted === undefined ? "" : `; accepted: ${first.accepted}`),
                    ...group
                        .sort((a, b) =>
                            `${a.target} ${a.variant.join(" ")}`.localeCompare(
                                `${b.target} ${b.variant.join(" ")}`,
                            ),
                        )
                        .map(findingLine),
                ];
            });
    };
    const fresh = findings.filter((finding) => finding.accepted === undefined);
    const crashes = fresh.filter((finding) => finding.run.crashed !== undefined);
    const rest = fresh.filter((finding) => finding.run.crashed === undefined);
    const lines = [
        ...header,
        "",
        "## Crashes",
        "",
        ...(crashes.length === 0 ? ["None."] : byOption(crashes)),
        "",
        "## Disagreements",
        "",
        ...(rest.length === 0 ? ["None."] : byOption(rest)),
        "",
        "## Accepted",
        "",
        ...byOption(findings.filter((finding) => finding.accepted !== undefined)),
        "",
        "## Not checked",
        "",
        "The samples whose checks clang's own compile rejects (a file the option names that does not exist, a language the checks are not written in):",
        "",
        ...[...unchecked]
            .sort(([a], [b]) => a.localeCompare(b))
            .map(([spelling, count]) => `- \`${spelling}\`: ${String(count)}`),
    ];
    return `${lines.join("\n")}\n`;
}

async function main(): Promise<void> {
    let values;
    try {
        ({ values } = parseArgs({
            options: {
                clice: { type: "string" },
                include: { type: "string" },
                clang: { type: "string" },
                only: { type: "string" },
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
    if (values.clice === undefined) {
        fail(`${USAGE}\nerror: --clice is required`);
    }
    const clice = path.resolve(values.clice);
    const { include, clang } = releaseTools(values.include, values.clang);
    const options = parseOptions(read(path.join(include, "clang", "Options", "Options.inc")));
    const classes = optionClasses(
        options,
        parseClassTable(read(path.join(REPO_ROOT, "src", "command", "option_classes.inc"))),
    );
    const only = values.only?.split(",").map((id) => id.trim());
    // An action is the compile's own choice: the query and clice's compile
    // pick theirs (in_query_view).
    const candidates = [...options.values()].filter((option) => {
        const chain = groupChain(options, option.id);
        if (only !== undefined) {
            return only.includes(option.id);
        }
        return (
            driverOption(option) &&
            classes.get(option.id) !== "LeftOut" &&
            !chain.includes("Action_Group") &&
            unprobed(option, chain, false) === undefined
        );
    });
    const version = (program: string): string =>
        spawnSync(program, ["--version"], { encoding: "utf8" }).stdout.split("\n")[0] ?? program;

    const scratch = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "clice-agreement-")));
    try {
        const prober = new Prober(
            clang,
            scratch,
            options,
            read(path.join(include, "clang", "Basic", "Features.def")),
        );
        const checker = new Checker(clice, clang, scratch, prober);
        let done = 0;
        const results = await Promise.all(
            candidates.map(async (option, index) => {
                const result = await checker.check(
                    option,
                    classes.get(option.id) ?? "Semantic",
                    probeTargets(option, groupChain(options, option.id)),
                    index,
                );
                done++;
                if (process.stderr.isTTY) {
                    process.stderr.write(`\rchecked ${String(done)}/${String(candidates.length)}`);
                }
                return { option, ...result };
            }),
        );
        const unchecked = new Map(
            results
                .filter((result) => result.unchecked > 0)
                .map((result) => [result.option.spelling, result.unchecked]),
        );
        const report = render(
            [
                "# clice against clang, on the options the table keeps",
                "",
                `${String(candidates.length)} options, each with its sample values on its targets, ` +
                    `clang ${version(clang)}, clice ${version(clice)}.`,
            ],
            results.flatMap((result) => result.findings),
            unchecked,
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
