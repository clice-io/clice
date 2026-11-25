/// Gathers the evidence for re-checking the classes src/command/option_classes.inc
/// gives clang's options at an LLVM upgrade: which of the driver's options are
/// new or changed, the class the table gives each, what a real clang does with
/// each (the macros and `__has_feature` answers it changes, the cc1 arguments
/// it adds or removes and which part of the CompilerInvocation keeps them),
/// and where the driver reads it for a decision of its own:
///
///   node tools/options/options.ts [--include <dir>] [--clang <clang>]
///       [--previous <Options.inc>] [--llvm <llvm-project>] [--only <ID,...>]
///       [--out <file>]
///
/// --include is the libclang include directory holding clang/Options/Options.inc
/// and clang/Basic/Features.def, --clang a clang of the same release; both
/// default to the host's xclang repositories in Bazel's output base. The
/// candidates are the driver's options: with --previous, those new or changed
/// against an older Options.inc; with --only, the options named; else all of
/// them. --llvm is an llvm-project checkout of the release, for the driver's
/// reads. The Markdown report goes to stdout, or to --out.

import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { parseArgs } from "node:util";
import { runProcess } from "../client/process.ts";
import { REPO_ROOT } from "../repo.ts";

const INVALID = "INVALID";

/// An option of clang's table, as Options.inc spells it.
export interface Option {
    id: string;
    /// With its prefix, as the table's comment writes it (`-fno-plt`).
    spelling: string;
    kind: string;
    /// INVALID for none.
    group: string;
    /// INVALID for none.
    alias: string;
    flags: string[];
    visibility: string[];
    /// The values the table lists for it; none where it lists them by code.
    values: string[] | undefined;
    /// The prefix of its *_OPTION_WITH_MARSHALLING entry: the part of the
    /// CompilerInvocation cc1 keeps it in (CODEGEN, LANG, ...).
    marshalling: string | undefined;
}

function literalEnd(text: string, open: number): number {
    const quote = text[open];
    for (let at = open + 1; at < text.length; at++) {
        if (text[at] === "\\") {
            at++;
        } else if (text[at] === quote) {
            return at;
        }
    }
    throw new Error(`a literal of Options.inc does not end: ${text.slice(open, open + 80)}`);
}

/// The top-level arguments of the macro call whose arguments start at
/// `start`, through string literals, comments and brackets.
function splitArguments(text: string, start: number): string[] {
    const fields: string[] = [];
    let field = start;
    let depth = 0;
    for (let at = start; at < text.length; at++) {
        const c = text[at];
        if (c === '"' || c === "'") {
            at = literalEnd(text, at);
        } else if (c === "/" && text[at + 1] === "*") {
            const end = text.indexOf("*/", at + 2);
            if (end < 0) {
                break;
            }
            at = end + 1;
        } else if (c === "(" || c === "{" || c === "[") {
            depth++;
        } else if (c === ")" || c === "}" || c === "]") {
            if (depth === 0) {
                fields.push(text.slice(field, at).trim());
                return fields;
            }
            depth--;
        } else if (c === "," && depth === 0) {
            fields.push(text.slice(field, at).trim());
            field = at + 1;
        }
    }
    throw new Error(`an entry of Options.inc does not end: ${text.slice(start, start + 80)}`);
}

function names(field: string): string[] {
    return [
        ...new Set(
            field
                .split("|")
                .map((each) => each.trim())
                .filter((each) => each !== "" && each !== "0"),
        ),
    ];
}

function stringLiteral(field: string): string | undefined {
    const literal = /^"((?:[^"\\]|\\.)*)"$/s.exec(field);
    return literal?.[1]?.replace(/\\(.)/g, "$1");
}

/// The options of a generated Options.inc by ID, each with the kind of its
/// marshalling.
export function parseOptions(text: string): Map<string, Option> {
    const options = new Map<string, Option>();
    const marshalling = new Map<string, string>();
    for (const entry of text.matchAll(/^(?:OPTION|([A-Z_]+)_OPTION_WITH_MARSHALLING)\(/gm)) {
        const fields = splitArguments(text, entry.index + entry[0].length);
        const [
            ,
            spelled = "",
            id = "",
            kind = "",
            group = "",
            alias = "",
            ,
            flags = "",
            visibility = "",
        ] = fields;
        const kindOfMarshalling = entry[1];
        if (kindOfMarshalling !== undefined) {
            marshalling.set(id, kindOfMarshalling);
            continue;
        }
        options.set(id, {
            id,
            spelling: /\/\* (.*) \*\//.exec(spelled)?.[1] ?? "",
            kind,
            group,
            alias,
            flags: names(flags),
            visibility: names(visibility),
            values: stringLiteral(fields[13] ?? "")?.split(","),
            marshalling: undefined,
        });
    }
    for (const [id, kind] of marshalling) {
        const option = options.get(id);
        if (option !== undefined) {
            option.marshalling = kind;
        }
    }
    return options;
}

/// A line of option_classes.inc: `OPTION_GROUP(GROUP, CLASS)` or
/// `OPTION(ID, CLASS)`.
export interface ClassLine {
    group: boolean;
    id: string;
    cls: string;
}

export function parseClassTable(text: string): ClassLine[] {
    const lines: ClassLine[] = [];
    for (const line of text.split("\n")) {
        const entry = /^\s*(OPTION_GROUP|OPTION)\(\s*(\w+)\s*,\s*(\w+)\s*\)/.exec(line);
        if (entry?.[2] !== undefined && entry[3] !== undefined) {
            lines.push({ group: entry[1] === "OPTION_GROUP", id: entry[2], cls: entry[3] });
        }
    }
    return lines;
}

/// The IDs an option matches as clang's Option::matches does: an alias
/// matches what the option it aliases matches, an option itself and every
/// group of its group chain.
export function groupChain(options: ReadonlyMap<string, Option>, id: string): string[] {
    let current = options.get(id);
    while (current !== undefined && current.alias !== INVALID) {
        current = options.get(current.alias);
    }
    const chain: string[] = [];
    while (current !== undefined && !chain.includes(current.id)) {
        chain.push(current.id);
        current = options.get(current.group);
    }
    return chain;
}

/// Each option's class as option_classes() in src/command/argument.cpp
/// computes it.
export function optionClasses(
    options: ReadonlyMap<string, Option>,
    lines: readonly ClassLine[],
): Map<string, string> {
    const classes = new Map<string, string>();
    for (const option of options.values()) {
        const leftOut = option.flags.includes("Ignored") || option.marshalling === "CODEGEN";
        classes.set(option.id, leftOut ? "LeftOut" : "Semantic");
    }
    const chains = [...options.keys()].map((id) => [id, groupChain(options, id)] as const);
    for (const { group, id, cls } of lines) {
        if (!options.has(id)) {
            throw new Error(`option_classes.inc names ${id}, which Options.inc does not have`);
        }
        if (!group) {
            classes.set(id, cls);
            continue;
        }
        for (const [member, chain] of chains) {
            if (chain.includes(id)) {
                classes.set(member, cls);
            }
        }
    }
    return classes;
}

/// An option a compiler command spells for the driver.
export function driverOption(option: Option): boolean {
    return (
        (option.visibility.includes("DefaultVis") || option.visibility.includes("CLOption")) &&
        option.alias === INVALID &&
        !["Group", "Input", "Unknown"].includes(option.kind)
    );
}

/// What makes an option a candidate against the previous release's table:
/// none for an option whose table entry is the same.
export function change(option: Option, previous: Option | undefined): string | undefined {
    if (previous === undefined) {
        return "new";
    }
    const set = (each: readonly string[]): string => [...each].sort().join(" | ") || "0";
    const fields: [string, string, string][] = [
        ["kind", previous.kind, option.kind],
        ["group", previous.group, option.group],
        ["alias", previous.alias, option.alias],
        ["flags", set(previous.flags), set(option.flags)],
        ["visibility", set(previous.visibility), set(option.visibility)],
        ["marshalling", previous.marshalling ?? "none", option.marshalling ?? "none"],
        ["values", set(previous.values ?? []), set(option.values ?? [])],
    ];
    const changed = fields
        .filter(([, was, is]) => was !== is)
        .map(([field, was, is]) => `${field} ${was} → ${is}`);
    return changed.length === 0 ? undefined : `changed ${changed.join(", ")}`;
}

/// The spelling an option and its negative counterpart share: `-fplt` for
/// `-fno-plt`, `/GR` for `/GR-`.
export function counterpartBase(spelling: string): string {
    for (const [negative, positive] of [
        ["-fno-", "-f"],
        ["-mno-", "-m"],
        ["-gno-", "-g"],
        ["--no-", "--"],
        ["-no-", "-"],
    ] as const) {
        if (spelling.startsWith(negative)) {
            return positive + spelling.slice(negative.length);
        }
    }
    return spelling.startsWith("/") && spelling.endsWith("-") ? spelling.slice(0, -1) : spelling;
}

/// Options merged with their counterparts, by the spelling they share.
export function families<T extends { option: Option }>(members: readonly T[]): Map<string, T[]> {
    const merged = new Map<string, T[]>();
    for (const member of members) {
        const base = counterpartBase(member.option.spelling);
        merged.set(base, [...(merged.get(base) ?? []), member]);
    }
    return merged;
}

export interface Target {
    name: string;
    triple: string;
    /// Probed in clang-cl's driver mode.
    cl: boolean;
}

const EVERY_TARGET: readonly Target[] = [
    ["x86", "x86_64-unknown-linux-gnu"],
    ["aarch64", "aarch64-unknown-linux-gnu"],
    ["arm", "armv7a-unknown-linux-gnueabihf"],
    ["riscv", "riscv64-unknown-linux-gnu"],
    ["ppc", "powerpc64le-unknown-linux-gnu"],
    ["mips", "mips64-unknown-linux-gnuabi64"],
    ["wasm", "wasm32-unknown-unknown"],
    ["mac", "arm64-apple-macosx14.0"],
    ["sparc", "sparcv9-unknown-linux-gnu"],
    ["loongarch", "loongarch64-unknown-linux-gnu"],
    ["systemz", "s390x-unknown-linux-gnu"],
    ["hexagon", "hexagon-unknown-linux-musl"],
    ["m68k", "m68k-unknown-linux-gnu"],
    ["amdgpu", "amdgcn-amd-amdhsa"],
    ["nvptx", "nvptx64-nvidia-cuda"],
    ["avr", "avr"],
    ["msp430", "msp430"],
    ["ve", "ve-unknown-linux-gnu"],
    ["x86-32", "i686-unknown-linux-gnu"],
    ["mingw", "x86_64-w64-windows-gnu"],
].map(([name = "", triple = ""]) => ({ name, triple, cl: false }));

const GENERIC_TARGETS = EVERY_TARGET.filter(({ name }) =>
    ["x86", "aarch64", "mac", "mingw"].includes(name),
);

const CL_TARGET: Target = { name: "cl", triple: "x86_64-pc-windows-msvc", cl: true };

/// The targets an option is probed on: cl's options in its driver mode, a
/// target's options on every target, the rest on a few, and in cl's driver
/// mode too when it takes them.
/// What every probe of a target runs clang with: the target in its driver
/// mode, and the language, C++ as clang-cl reads it or C++20.
export function targetArguments(target: Target): { driver: string[]; language: string[] } {
    return {
        driver: [
            ...(target.cl ? ["--driver-mode=cl"] : []),
            "--no-default-config",
            `--target=${target.triple}`,
        ],
        language: target.cl ? ["/TP"] : ["-x", "c++", "-std=c++20"],
    };
}

export function probeTargets(option: Option, chain: readonly string[]): readonly Target[] {
    const cl = option.visibility.includes("CLOption");
    if (option.spelling.startsWith("/") || (cl && !option.visibility.includes("DefaultVis"))) {
        return [CL_TARGET];
    }
    const targets =
        option.flags.includes("TargetSpecific") ||
        chain.includes("m_Group") ||
        option.spelling.startsWith("-m")
            ? EVERY_TARGET
            : GENERIC_TARGETS;
    return cl ? [...targets, CL_TARGET] : targets;
}

/// The values cl's own options commonly take (`/guard:cf`, `/fp:fast`,
/// `/arch:AVX2`, `/Zc:inline`).
const CL_SAMPLE = [
    "1",
    "2",
    "3",
    "4",
    "-",
    "cf",
    "cf-",
    "ehcont",
    "s",
    "t",
    "d",
    "fast",
    "strict",
    "precise",
    "except",
    "AVX2",
    "AVX512",
    "IA32",
    "c++20",
    "c++latest",
    "c17",
    "inline",
    "strictStrings",
];

/// The arguments an option is probed with: a value of those the table
/// lists, or a sample of the values options commonly take.
export function variants(option: Option, file: string, previous?: Option): string[][] {
    if (option.kind === "Flag") {
        return [[option.spelling]];
    }
    const added =
        previous === undefined
            ? []
            : (option.values ?? []).filter((value) => !(previous.values ?? []).includes(value));
    const listed = option.values && [...new Set([...added, ...option.values.slice(0, 8)])];
    const sample = option.spelling.startsWith("/")
        ? [...CL_SAMPLE, file]
        : [
              "1",
              "2",
              "none",
              "all",
              "default",
              "full",
              file,
              "foo",
              "fast",
              "on",
              "generic",
              "hidden",
              "static",
          ];
    const values = listed ?? sample;
    return values.map((value) =>
        option.kind === "Separate" ? [option.spelling, value] : [option.spelling + value],
    );
}

/// Why an option goes unprobed, none for one probed. The target features
/// are many and plainly Semantic: they are probed only when named.
export function unprobed(
    option: Option,
    chain: readonly string[],
    named: boolean,
): string | undefined {
    if (
        ["RemainingArgs", "RemainingArgsJoined", "MultiArg", "JoinedAndSeparate"].includes(
            option.kind,
        )
    ) {
        return `a ${option.kind} option`;
    }
    if (!named && chain.some((group) => /_Features\w*_Group$/.test(group))) {
        return "a target feature";
    }
    return undefined;
}

/// The arguments of the first job `-###` prints.
export function compileJob(stderr: string): string[] | undefined {
    const line = stderr.split(/\r?\n/).find((each) => each.startsWith(' "'));
    return line === undefined
        ? undefined
        : [...line.matchAll(/"((?:[^"\\]|\\.)*)"/g)].map(([, token = ""]) =>
              token.replace(/\\(.)/g, "$1"),
          );
}

/// The options cc1 takes, as its parser finds them in an argument.
export interface Cc1Table {
    exact: Map<string, Option>;
    /// Longest spelling first.
    joined: Option[];
}

export function cc1Table(options: Iterable<Option>): Cc1Table {
    const exact = new Map<string, Option>();
    const joined: Option[] = [];
    for (const option of options) {
        if (!option.visibility.includes("CC1Option") || option.kind === "Group") {
            continue;
        }
        const known = exact.get(option.spelling);
        if (
            ["Flag", "Separate", "JoinedOrSeparate", "MultiArg"].includes(option.kind) &&
            (known === undefined || (known.alias !== INVALID && option.alias === INVALID))
        ) {
            exact.set(option.spelling, option);
        }
        if (
            ["Joined", "CommaJoined", "JoinedOrSeparate", "JoinedAndSeparate"].includes(option.kind)
        ) {
            joined.push(option);
        }
    }
    joined.sort((a, b) => b.spelling.length - a.spelling.length);
    return { exact, joined };
}

/// A cc1 argument, with its value when the option takes it apart, and the
/// part of the CompilerInvocation keeping it: MANUAL:<ID> for an option
/// CompilerInvocation reads by hand, MANUAL:<argument> for one cc1 does not
/// have.
export interface Cc1Argument {
    text: string;
    kind: string;
}

export function cc1Arguments(
    options: ReadonlyMap<string, Option>,
    table: Cc1Table,
    tokens: readonly string[],
): Cc1Argument[] {
    const parsed: Cc1Argument[] = [];
    const kind = (option: Option | undefined, text: string): string => {
        if (option === undefined) {
            return `MANUAL:${text}`;
        }
        const id = option.alias === INVALID ? option.id : option.alias;
        return options.get(id)?.marshalling ?? `MANUAL:${id}`;
    };
    for (let at = 0; at < tokens.length; at++) {
        const token = tokens[at] ?? "";
        const exact = table.exact.get(token);
        const value = tokens[at + 1];
        if (exact !== undefined) {
            const separate = exact.kind === "Separate" || exact.kind === "JoinedOrSeparate";
            const text = separate && value !== undefined ? `${token} ${value}` : token;
            at += text === token ? 0 : 1;
            parsed.push({ text, kind: kind(exact, text) });
            continue;
        }
        const joined = table.joined.find(({ spelling }) => token.startsWith(spelling));
        parsed.push({ text: token, kind: kind(joined, token) });
    }
    return parsed;
}

/// `from` without `minus`, as multisets.
export function difference(
    from: readonly Cc1Argument[],
    minus: readonly Cc1Argument[],
): Cc1Argument[] {
    const counts = new Map<string, number>();
    for (const { text } of minus) {
        counts.set(text, (counts.get(text) ?? 0) + 1);
    }
    return from.filter(({ text }) => {
        const count = counts.get(text) ?? 0;
        counts.set(text, count - 1);
        return count <= 0;
    });
}

/// What a real clang did with an option's sample values.
export interface Evidence {
    /// The targets some value compiled on.
    accepted: string[];
    /// The macro definitions (`+` added, `-` removed) a value changes.
    macros: Set<string>;
    /// The `__has_feature` and `__has_extension` answers a value changes.
    features: Set<string>;
    /// The cc1 arguments a value adds or removes, with their kinds.
    added: Map<string, string>;
    removed: Map<string, string>;
    /// The first error of a rejected value.
    rejection: string | undefined;
}

export type Bucket = "A" | "B" | "C" | "D" | "E";

export const BUCKETS: Record<Bucket, string> = {
    A: "a macro or `__has_feature` answer changes",
    B: "no sample value is accepted",
    C: "no cc1 argument changes",
    D: "only code generation, output and analyzer arguments change",
    E: "other cc1 arguments change",
};

/// The order a family takes the bucket of its most telling member in.
const BUCKET_PRIORITY: readonly Bucket[] = ["A", "E", "B", "D", "C"];

const BUCKET_ORDER = ["A", "B", "C", "D", "E"] as const;

const OUTPUT_KINDS = new Set([
    "CODEGEN",
    "DEPENDENCY_OUTPUT",
    "ANALYZER",
    "SSAF",
    "MIGRATOR",
    "PREPROCESSOR_OUTPUT",
]);

/// What changes only how cc1 runs and reports, beside its outputs.
const PRESENTATION_KINDS = new Set([...OUTPUT_KINDS, "FRONTEND", "DIAG"]);

function kinds(evidence: Evidence): Set<string> {
    return new Set([...evidence.added.values(), ...evidence.removed.values()]);
}

export function bucket(evidence: Evidence): Bucket {
    if (evidence.accepted.length === 0) {
        return "B";
    }
    if (evidence.macros.size > 0 || evidence.features.size > 0) {
        return "A";
    }
    const changed = kinds(evidence);
    if (changed.size === 0) {
        return "C";
    }
    return [...changed].every((kind) => OUTPUT_KINDS.has(kind)) ? "D" : "E";
}

/// What a family of options changes: the bucket of its most telling member
/// and the kinds of the cc1 arguments its members change; none for a family
/// none of whose members was probed.
export interface Finding {
    bucket: Bucket;
    kinds: Set<string>;
}

export function finding(evidence: readonly Evidence[]): Finding | undefined {
    const found = evidence.map(bucket);
    const most = BUCKET_PRIORITY.find((each) => found.includes(each));
    return most === undefined
        ? undefined
        : { bucket: most, kinds: new Set(evidence.flatMap((each) => [...kinds(each)])) };
}

/// Whether what an option's family changes contradicts its class: a LeftOut
/// option whose family changes what clice's analysis reads, or a Semantic one
/// whose family changes nothing it reads. A spelling undoes its counterpart,
/// so the two take one class.
export function suspicious(cls: string, { bucket: found, kinds: changed }: Finding): boolean {
    if (cls === "LeftOut") {
        return (
            found === "A" ||
            (found === "E" && [...changed].some((kind) => !PRESENTATION_KINDS.has(kind)))
        );
    }
    return cls === "Semantic" && (found === "C" || found === "D");
}

/// An option of the report: a candidate, or the counterpart of one.
export interface Member {
    option: Option;
    cls: string;
    /// What makes it a candidate against the previous table, or that it is a
    /// candidate's counterpart.
    note: string | undefined;
    evidence: Evidence | undefined;
    /// Why it went unprobed.
    unprobed: string | undefined;
    /// The files of clang's driver reading it for a decision of its own.
    reads: readonly string[];
}

/// The files under clang/lib/Driver that name each option, beside the two
/// that forward options to cc1 and fc1.
export function driverReads(llvm: string): Map<string, string[]> {
    const driver = path.join(llvm, "clang", "lib", "Driver");
    const reads = new Map<string, string[]>();
    for (const entry of fs.readdirSync(driver, { recursive: true, encoding: "utf8" }).sort()) {
        const file = entry.split(path.sep).join("/");
        if (
            !/\.(cpp|h)$/.test(file) ||
            ["ToolChains/Clang.cpp", "ToolChains/Flang.cpp"].includes(file)
        ) {
            continue;
        }
        const named = fs.readFileSync(path.join(driver, entry), "utf8").matchAll(/\bOPT_(\w+)/g);
        for (const id of new Set([...named].map(([, id = ""]) => id))) {
            reads.set(id, [...(reads.get(id) ?? []), file]);
        }
    }
    return reads;
}

function listed(items: readonly string[], limit: number): string {
    const shown = items.slice(0, limit).join(", ");
    return items.length > limit ? `${shown} (+${items.length - limit} more)` : shown;
}

function signed(items: Iterable<string>): string[] {
    return [...items].sort().map((item) => `${item.slice(0, 1)}\`${item.slice(1)}\``);
}

function cc1Changes(added: Map<string, string>, removed: Map<string, string>): string[] {
    const each = (sign: string, map: Map<string, string>): string[] =>
        [...map]
            .sort(([a], [b]) => a.localeCompare(b))
            .map(([text, kind]) => `${sign}\`${text}\` ${kind}`);
    return [...each("+", added), ...each("-", removed)];
}

function memberLines(member: Member): string[] {
    const { option, cls, note, evidence, reads } = member;
    const head = [`\`${option.spelling}\` ${option.id}: ${cls}`];
    if (option.marshalling !== undefined) {
        head.push(`${option.marshalling}-marshalled`);
    }
    if (note !== undefined) {
        head.push(note);
    }
    if (evidence === undefined) {
        head.push(member.unprobed ?? "not probed");
    } else if (evidence.accepted.length > 0) {
        head.push(`bucket ${bucket(evidence)} on ${evidence.accepted.join(" ")}`);
    } else {
        head.push(`bucket B: ${evidence.rejection ?? "rejected"}`);
    }
    const lines = [`- ${head.join("; ")}`];
    if (evidence !== undefined) {
        if (evidence.macros.size > 0) {
            lines.push(`  - macros: ${listed(signed(evidence.macros), 12)}`);
        }
        if (evidence.features.size > 0) {
            lines.push(`  - features: ${listed(signed(evidence.features), 8)}`);
        }
        const changed = cc1Changes(evidence.added, evidence.removed);
        if (changed.length > 0) {
            lines.push(`  - cc1: ${listed(changed, 10)}`);
        }
    }
    if (reads.length > 0) {
        lines.push(`  - driver: ${reads.join(", ")}`);
    }
    return lines;
}

/// The report: a summary line per bucket, the members whose evidence
/// contradicts their class, then each family under the bucket of its most
/// telling member.
export function render(header: readonly string[], members: readonly Member[]): string {
    const sections = new Map<Bucket | "unprobed", [string, Member[]][]>();
    const contradicted: string[] = [];
    for (const [base, family] of [...families(members)].sort(([a], [b]) => a.localeCompare(b))) {
        const found = finding(family.flatMap(({ evidence }) => evidence ?? []));
        const section = found?.bucket ?? "unprobed";
        const ordered = [...family].sort(
            (a, b) =>
                Number(a.option.spelling !== base) - Number(b.option.spelling !== base) ||
                a.option.spelling.localeCompare(b.option.spelling),
        );
        sections.set(section, [...(sections.get(section) ?? []), [base, ordered]]);
        if (found !== undefined) {
            contradicted.push(
                ...ordered
                    .filter(({ cls }) => suspicious(cls, found))
                    .map(
                        ({ option, cls }) =>
                            `- \`${option.spelling}\` ${option.id}: ${cls}, ${found.bucket}`,
                    ),
            );
        }
    }
    const out = [...header, ""];
    for (const each of BUCKET_ORDER) {
        out.push(`- ${each}, ${BUCKETS[each]}: ${sections.get(each)?.length ?? 0}`);
    }
    out.push(`- not probed: ${sections.get("unprobed")?.length ?? 0}`, "", "## Suspicious", "");
    out.push(
        "By the bucket of the option's family: LeftOut with A, or with E through arguments " +
            "other than output and presentation ones; Semantic with C or D, candidates to drop.",
        "",
        ...(contradicted.length === 0 ? ["None."] : contradicted),
    );
    for (const each of [...BUCKET_ORDER, "unprobed"] as const) {
        const section = sections.get(each);
        if (section === undefined) {
            continue;
        }
        out.push("", each === "unprobed" ? "## Not probed" : `## ${each}, ${BUCKETS[each]}`);
        for (const [base, family] of section) {
            out.push("", `### ${base}`, "", ...family.flatMap(memberLines));
        }
    }
    return `${out.join("\n")}\n`;
}

/// A compiler run that takes longer hangs: some options make clang wait.
const COMPILE_TIMEOUT = 60_000;

/// The compiler runs at once.
const JOBS = 8;

export interface Ran {
    status: number | null;
    stdout: string;
    stderr: string;
}

/// Runs `task`s at most `limit` at a time.
export function limiter(limit: number): <T>(task: () => Promise<T>) => Promise<T> {
    let active = 0;
    const waiting: (() => void)[] = [];
    return async (task) => {
        if (active < limit) {
            active++;
        } else {
            await new Promise<void>((resolve) => waiting.push(resolve));
        }
        try {
            return await task();
        } finally {
            const next = waiting.shift();
            if (next === undefined) {
                active--;
            } else {
                next();
            }
        }
    };
}

/// A compiler's run with what it printed, undecodable bytes replaced: some
/// options make it print binary.
export async function compile(clang: string, args: readonly string[], cwd: string): Promise<Ran> {
    const ran = await runProcess(clang, args, { cwd, timeout: COMPILE_TIMEOUT });
    let stderr = ran.stderr;
    if (ran.error !== undefined) {
        stderr += `\nerror: ${ran.error.message}`;
    } else if (ran.signal !== null) {
        stderr += `\nerror: killed by ${ran.signal}`;
    }
    return { status: ran.status, stdout: ran.stdout, stderr };
}

function firstError(stderr: string): string | undefined {
    return stderr
        .split(/\r?\n/)
        .find((line) => line.includes("error:"))
        ?.trim();
}

/// Why a run failed: its first error, or what it printed first where it
/// reports none (LLVM's option parser).
function failure(ran: Ran): string {
    const printed = ran.stderr.split(/\r?\n/).find((line) => line.trim() !== "");
    return firstError(ran.stderr) ?? printed?.trim() ?? `exit ${ran.status ?? "without status"}`;
}

export interface Compiled {
    cc1: Cc1Argument[];
    /// None where the run printed no macro dump or feature answers: an option
    /// sending the output elsewhere (`-o`) or printing another (`-M`).
    macros: Set<string> | undefined;
    features: Set<string> | undefined;
}

/// Runs a clang on each option's sample values, a few runs at a time, against
/// each target's compile without them.
export class Prober {
    private readonly run = limiter(JOBS);
    private readonly clang: string;
    private readonly scratch: string;
    private readonly options: ReadonlyMap<string, Option>;
    private readonly input: string;
    private readonly featureFile: string;
    private readonly cc1: Cc1Table;
    private readonly baselines = new Map<string, Promise<Compiled | string>>();

    constructor(
        clang: string,
        scratch: string,
        options: ReadonlyMap<string, Option>,
        featuresDef: string,
    ) {
        this.clang = clang;
        this.scratch = scratch;
        this.options = options;
        this.input = path.join(scratch, "t.cpp");
        this.featureFile = path.join(scratch, "features.cpp");
        fs.writeFileSync(this.input, "int main(){}\n");
        fs.writeFileSync(this.featureFile, featureProbes(featuresDef));
        this.cc1 = cc1Table(options.values());
    }

    /// What a target's compile is without the option, or why it fails.
    baseline(target: Target): Promise<Compiled | string> {
        const key = `${target.triple} ${target.cl}`;
        let baseline = this.baselines.get(key);
        if (baseline === undefined) {
            baseline = this.compiled(target, [], this.scratch);
            this.baselines.set(key, baseline);
        }
        return baseline;
    }

    /// The compile of `args` on a target in `cwd`, or why it is no evidence:
    /// its cc1 job is rejected, or its preprocessor runs fail.
    async compiled(
        target: Target,
        args: readonly string[],
        cwd: string,
    ): Promise<Compiled | string> {
        const { driver, language } = targetArguments(target);
        const job = await this.run(() =>
            compile(
                this.clang,
                [
                    ...driver,
                    "-###",
                    ...(target.cl ? ["/Zs"] : ["-fsyntax-only"]),
                    ...language,
                    ...args,
                    this.input,
                ],
                cwd,
            ),
        );
        const tokens = job.status === 0 ? compileJob(job.stderr) : undefined;
        if (tokens === undefined || firstError(job.stderr) !== undefined) {
            return failure(job);
        }
        const preprocess = (extra: readonly string[], file: string): Promise<Ran> =>
            this.run(() =>
                compile(this.clang, [...driver, ...language, ...extra, ...args, file], cwd),
            );
        const [macros, features] = await Promise.all([
            preprocess(target.cl ? ["/E", "/clang:-dM"] : ["-E", "-dM"], this.input),
            preprocess(target.cl ? ["/E"] : ["-E", "-P"], this.featureFile),
        ]);
        if (macros.status !== 0) {
            return failure(macros);
        }
        if (features.status !== 0) {
            return failure(features);
        }
        const lines = (stdout: string): string[] =>
            stdout.split(/\r?\n/).filter((line) => line.trim() !== "" && !line.startsWith("#"));
        const defines = macros.stdout.split(/\r?\n/).filter((line) => line.startsWith("#define "));
        const answers = lines(features.stdout).filter((line) => /^[FE]_\w+$/.test(line));
        return {
            cc1: cc1Arguments(
                this.options,
                this.cc1,
                tokens.slice(2).filter((token) => token !== this.input),
            ),
            macros: defines.length === 0 ? undefined : new Set(defines),
            features:
                answers.length === 0
                    ? undefined
                    : new Set(
                          answers.map((line) =>
                              line.replace(/^([FE])_(\w+)$/, (_, which: string, name: string) =>
                                  which === "F"
                                      ? `__has_feature(${name})`
                                      : `__has_extension(${name})`,
                              ),
                          ),
                      ),
        };
    }

    async probe(
        option: Option,
        targets: readonly Target[],
        index: number,
        previous: Option | undefined,
    ): Promise<Evidence> {
        const directory = path.join(this.scratch, String(index));
        fs.mkdirSync(directory);
        const file = path.join(directory, "f.txt");
        fs.writeFileSync(file, "");
        const shown = (text: string): string =>
            text.replaceAll(file, "<file>").replaceAll(this.scratch, "<tmp>");
        const perTarget = await Promise.all(
            targets.map(async (target) => {
                const baseline = await this.baseline(target);
                if (typeof baseline === "string") {
                    throw new Error(`${this.clang} fails on ${target.triple}: ${baseline}`);
                }
                const results: (Compiled | string)[] = [];
                for (const args of variants(option, file, previous)) {
                    results.push(await this.compiled(target, args, directory));
                }
                return { target, baseline, results };
            }),
        );
        const evidence: Evidence = {
            accepted: [],
            macros: new Set(),
            features: new Set(),
            added: new Map(),
            removed: new Map(),
            rejection: undefined,
        };
        for (const { target, baseline, results } of perTarget) {
            for (const result of results) {
                if (typeof result === "string") {
                    evidence.rejection ??= shown(result);
                    continue;
                }
                if (!evidence.accepted.includes(target.name)) {
                    evidence.accepted.push(target.name);
                }
                const changes = (
                    now: Set<string> | undefined,
                    before: Set<string> | undefined,
                    into: Set<string>,
                ): void => {
                    if (now === undefined || before === undefined) {
                        return;
                    }
                    for (const [sign, from, minus] of [
                        ["+", now, before],
                        ["-", before, now],
                    ] as const) {
                        for (const each of from) {
                            if (!minus.has(each)) {
                                into.add(sign + shown(each.replace(/^#define /, "")));
                            }
                        }
                    }
                };
                changes(result.macros, baseline.macros, evidence.macros);
                changes(result.features, baseline.features, evidence.features);
                for (const { text, kind } of difference(result.cc1, baseline.cc1)) {
                    evidence.added.set(shown(text), kind);
                }
                for (const { text, kind } of difference(baseline.cc1, result.cc1)) {
                    evidence.removed.set(shown(text), kind);
                }
            }
        }
        return evidence;
    }
}

/// A file whose preprocessed text holds `F_<name>` for each feature
/// `__has_feature` answers and `E_<name>` for each `__has_extension` does.
export function featureProbes(featuresDef: string): string {
    const features = new Set(
        [...featuresDef.matchAll(/^\s*(?:FEATURE|EXTENSION)\((\w+)/gm)].map(
            ([, name = ""]) => name,
        ),
    );
    return [...features]
        .sort()
        .map(
            (name) =>
                `#if __has_feature(${name})\nF_${name}\n#endif\n` +
                `#if __has_extension(${name})\nE_${name}\n#endif\n`,
        )
        .join("");
}

export function fail(message: string): never {
    console.error(message);
    process.exit(1);
}

const USAGE =
    "usage: node tools/options/options.ts [--include <dir>] [--clang <clang>] " +
    "[--previous <Options.inc>] [--llvm <llvm-project>] [--only <ID,...>] [--out <file>]";

function usageError(message: string): never {
    console.error(`${USAGE}\nerror: ${message}`);
    process.exit(2);
}

function parseCommandLine() {
    try {
        return parseArgs({
            options: {
                include: { type: "string" },
                clang: { type: "string" },
                previous: { type: "string" },
                llvm: { type: "string" },
                only: { type: "string" },
                out: { type: "string" },
                help: { type: "boolean", short: "h" },
            },
        });
    } catch (error) {
        usageError(error instanceof Error ? error.message : String(error));
    }
}

/// The triple of this host's xclang repositories.
function hostTriple(): string | undefined {
    const arch = new Map([
        ["x64", "x86_64"],
        ["arm64", "aarch64"],
    ]).get(process.arch);
    const system = new Map([
        ["linux", "unknown-linux-gnu"],
        ["darwin", "apple-darwin"],
    ]).get(process.platform);
    return arch === undefined || system === undefined ? undefined : `${arch}-${system}`;
}

function bazelExternal(): string {
    const result = spawnSync("npx", ["bazel", "info", "output_base"], {
        cwd: REPO_ROOT,
        encoding: "utf8",
        stdio: ["ignore", "pipe", "inherit"],
    });
    if (result.status !== 0) {
        fail(
            `npx bazel info output_base: ${result.error?.message ?? `exit ${result.status ?? "?"}`}`,
        );
    }
    return path.join(result.stdout.trim(), "external");
}

/// The libclang include directory and the clang of the release: those given,
/// else the host's xclang repositories in Bazel's output base.
export function releaseTools(
    include: string | undefined,
    clang: string | undefined,
): { include: string; clang: string } {
    // The probes run in directories of their own: a path relative to here
    // would name nothing there, a bare name still goes through PATH.
    if (clang !== undefined && path.basename(clang) !== clang) {
        clang = path.resolve(clang);
    }
    if (include !== undefined && clang !== undefined) {
        return { include, clang };
    }
    const triple =
        hostTriple() ?? fail(`no xclang for ${process.platform}: pass --include and --clang`);
    const external = bazelExternal();
    const fetched: string[] = [];
    if (include === undefined) {
        include = path.join(external, `xclang++xclang+libclang_${triple}`, "include");
        fetched.push(include);
    }
    if (clang === undefined) {
        clang = path.join(external, `xclang++xclang+xclang_${triple}`, "bin", "clang");
        fetched.push(clang);
    }
    for (const file of fetched) {
        if (!fs.existsSync(file)) {
            fail(`${file} is missing: build clice first, or pass --include and --clang`);
        }
    }
    return { include, clang };
}

export function read(file: string): string {
    try {
        return fs.readFileSync(file, "utf8");
    } catch (error) {
        fail(error instanceof Error ? error.message : String(error));
    }
}

async function main(): Promise<void> {
    const { values } = parseCommandLine();
    if (values.help === true) {
        console.log(USAGE);
        return;
    }
    const { include, clang } = releaseTools(values.include, values.clang);
    const version = spawnSync(clang, ["--version"], { encoding: "utf8" });
    if (version.status !== 0) {
        fail(`${clang} --version: ${version.error?.message ?? version.stderr}`);
    }
    const options = parseOptions(read(path.join(include, "clang", "Options", "Options.inc")));
    const classes = optionClasses(
        options,
        parseClassTable(read(path.join(REPO_ROOT, "src", "command", "option_classes.inc"))),
    );
    const previous =
        values.previous === undefined ? undefined : parseOptions(read(values.previous));
    if (previous?.size === 0) {
        fail(`${values.previous ?? ""} holds no option of clang's table`);
    }
    const only = values.only?.split(",").map((id) => id.trim());
    for (const id of only ?? []) {
        const kind = options.get(id)?.kind;
        if (kind === undefined || ["Group", "Input", "Unknown"].includes(kind)) {
            usageError(`--only: ${id} is no option of the driver's table`);
        }
    }
    const notes = new Map<string, string | undefined>();
    for (const option of options.values()) {
        if (only !== undefined ? only.includes(option.id) : driverOption(option)) {
            const note =
                previous === undefined ? undefined : change(option, previous.get(option.id));
            if (only !== undefined || previous === undefined || note !== undefined) {
                notes.set(option.id, note);
            }
        }
    }
    const reads = values.llvm === undefined ? undefined : driverReads(values.llvm);
    const candidates = notes.size;
    const bases = new Set(
        [...notes.keys()].map((id) => counterpartBase(options.get(id)?.spelling ?? "")),
    );
    for (const option of options.values()) {
        if (
            driverOption(option) &&
            !notes.has(option.id) &&
            bases.has(counterpartBase(option.spelling))
        ) {
            notes.set(option.id, "a candidate's counterpart");
        }
    }

    const scratch = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "clice-options-")));
    try {
        const prober = new Prober(
            clang,
            scratch,
            options,
            read(path.join(include, "clang", "Basic", "Features.def")),
        );
        let done = 0;
        const members = await Promise.all(
            [...notes].map(async ([id, note], index): Promise<Member> => {
                const option = options.get(id) ?? fail(`no option ${id}`);
                const chain = groupChain(options, id);
                const why = unprobed(option, chain, only !== undefined);
                const evidence =
                    why === undefined
                        ? await prober.probe(
                              option,
                              probeTargets(option, chain),
                              index,
                              previous?.get(id),
                          )
                        : undefined;
                done++;
                if (process.stderr.isTTY) {
                    process.stderr.write(`\rprobed ${done}/${notes.size}`);
                }
                return {
                    option,
                    cls: classes.get(id) ?? "Semantic",
                    note,
                    evidence,
                    unprobed: why,
                    reads: reads?.get(id) ?? [],
                };
            }),
        );
        if (process.stderr.isTTY) {
            process.stderr.write("\n");
        }
        const chosen =
            only !== undefined
                ? "named by --only"
                : previous !== undefined
                  ? `new or changed against ${values.previous ?? ""}`
                  : "every option of the driver";
        const header = [
            "# The classes of clang's options to check",
            "",
            `${candidates} candidates, ${chosen}, and ${notes.size - candidates} of their counterparts, ` +
                "each with its class from src/command/option_classes.inc, " +
                `probed with ${version.stdout.split("\n")[0] ?? clang}.` +
                (values.llvm === undefined
                    ? ""
                    : " `driver` lists the files of clang/lib/Driver, beside ToolChains/Clang.cpp and Flang.cpp, that read the option."),
        ];
        const report = render(header, members);
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
