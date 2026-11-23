/// Regenerates modules/ from modules/demand.cpp and the libraries' headers.
///
/// Every configuration CI builds is generated on one x64 Linux machine, which
/// cross-compiles the others (macOS against Apple's SDK, which MODULE.bazel
/// accepts the license of), then all of them are merged into modules/:
///
///   node tools/modules/modules.ts [--clice <path>] generate [<configuration>...]
///   node tools/modules/modules.ts [--clice <path>] merge
///
/// generate without a configuration generates every one. A configuration's
/// wrapping lands in build/modules/<configuration>; merge reads every one of
/// them there. Any clice runs it, by default the one the tree builds; after a
/// library's upgrade, one built before it, as the new headers need not build
/// the old modules/.

import { spawnSync, type StdioOptions } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { parseArgs } from "node:util";
import { REPO_ROOT } from "../repo.ts";

/// Each configuration's wrapping, its work directory beside it, and the
/// merge file.
const OUT = path.join(REPO_ROOT, "build", "modules");

/// The machine every configuration is generated on; it cross-compiles the
/// others.
const HOST_TRIPLE = "x86_64-unknown-linux-gnu";

export interface Configuration {
    name: string;
    buildType: "RelWithDebInfo" | "Debug";
    triple: string;
    /// A preprocessor condition holding in its compilations alone.
    condition: string;
}

export const CONFIGURATIONS: readonly Configuration[] = [
    {
        name: "linux-x64",
        buildType: "RelWithDebInfo",
        triple: "x86_64-unknown-linux-gnu",
        condition: "defined(__linux__) && defined(__x86_64__) && defined(NDEBUG)",
    },
    {
        name: "linux-x64-debug",
        buildType: "Debug",
        triple: "x86_64-unknown-linux-gnu",
        condition: "defined(__linux__) && defined(__x86_64__) && !defined(NDEBUG)",
    },
    {
        name: "linux-arm64",
        buildType: "RelWithDebInfo",
        triple: "aarch64-unknown-linux-gnu",
        condition: "defined(__linux__) && defined(__aarch64__) && defined(NDEBUG)",
    },
    {
        name: "macos-arm64",
        buildType: "RelWithDebInfo",
        triple: "aarch64-apple-darwin",
        condition: "defined(__APPLE__) && defined(__aarch64__) && defined(NDEBUG)",
    },
    {
        name: "macos-arm64-debug",
        buildType: "Debug",
        triple: "aarch64-apple-darwin",
        condition: "defined(__APPLE__) && defined(__aarch64__) && !defined(NDEBUG)",
    },
    {
        name: "macos-x64",
        buildType: "RelWithDebInfo",
        triple: "x86_64-apple-darwin",
        condition: "defined(__APPLE__) && defined(__x86_64__) && defined(NDEBUG)",
    },
    {
        name: "windows-x64",
        buildType: "RelWithDebInfo",
        triple: "x86_64-w64-mingw32",
        condition: "defined(_WIN32) && defined(__x86_64__) && defined(NDEBUG)",
    },
    {
        name: "windows-arm64",
        buildType: "RelWithDebInfo",
        triple: "aarch64-w64-mingw32",
        condition: "defined(_WIN32) && defined(__aarch64__) && defined(NDEBUG)",
    },
];

/// The Bazel arguments building //modules:demand for a configuration with
/// its compile command, the build's events written to `events`.
export function buildArguments(configuration: Configuration, events: string): string[] {
    return [
        "build",
        `--config=${configuration.buildType}`,
        ...(configuration.triple === HOST_TRIPLE
            ? []
            : [`--platforms=@xclang//platforms:${configuration.triple}`]),
        "--aspects=@compdb//:compdb.bzl%compdb",
        // demand.cpp compiled for real, as clice indexes a unit that does not
        // compile; the libraries' own sources not at all.
        "--output_groups=compdb,compilation_outputs",
        `--build_event_json_file=${events}`,
        "//modules:demand",
    ];
}

/// The compile command fragments of demand.cpp a build's events name, as
/// their URIs spell the paths.
export function compdbFragments(events: string): string[] {
    const fragments = new Set<string>();
    for (const [, fragment] of events.matchAll(
        /"uri":"file:\/\/([^"]*\/modules\/demand\.compdb\.json)"/g,
    )) {
        if (fragment !== undefined) {
            fragments.add(fragment);
        }
    }
    return [...fragments];
}

/// demand.cpp's compilation database from its fragment, which spells the
/// execution root as a placeholder inside JSON strings.
export function compileCommands(fragment: string, executionRoot: string): string {
    const escaped = executionRoot.replaceAll("\\", "\\\\");
    // A function, as a replacement string reads the `$` patterns in a path.
    return fragment.replaceAll("__COMPDB_EXECROOT__", () => escaped);
}

/// A module of `clice modularize --partition`, by the globs of its files.
export interface PartitionModule {
    name: string;
    files: string[];
    external?: boolean;
    textual?: boolean;
    provides?: string;
}

function repositoryGlobs(external: string, name: string): string[] {
    const link = `${external}/${name}`;
    return [...new Set([`${link}/**`, `${fs.realpathSync(link)}/**`])].sort();
}

/// The libraries' partition on this machine: Bazel's output base and the
/// repository contents cache differ by platform, so the globs come from
/// where the build put each library. The index reads the compiler's own
/// headers from clice's resource directory.
export function partition(outputBase: string, clice: string): { modules: PartitionModule[] } {
    const external = `${outputBase}/external`;
    const xclang = `${external}/xclang++xclang+xclang_*`;
    const libclang = `${external}/xclang++xclang+libclang_*`;
    const libraries = [
        ...["llvm", "llvm-c", "clang", "clang-c", "clang-tidy"].map(
            (tree) => `${libclang}/include/${tree}/**`,
        ),
        ...["simdjson+", "kotatsu+", "croaring+", "tomlplusplus+", "lmdb+"].flatMap((name) =>
            repositoryGlobs(external, name),
        ),
    ];
    return {
        modules: [
            { name: "libc", files: [`${xclang}/**/include/c++/v1/*.h`] },
            {
                name: "std",
                // <triple>/c++/v1 holds the target's __config_site.
                files: [`${xclang}/**/include/c++/v1/**`, `${xclang}/**/include/*/c++/v1/**`],
                external: true,
            },
            {
                name: "libc",
                files: [
                    `${xclang}/**`,
                    `${libclang}/lib/clang/**`,
                    `${path.dirname(path.dirname(clice))}/lib/clang/**`,
                ],
                textual: true,
                provides: "std.compat",
            },
            { name: "third_party", files: libraries },
        ],
    };
}

/// The files `<directory>/**/<tail>` matches, past no hidden entry and
/// through symbolic links: Bazel's repository contents cache makes
/// repositories of them, and node's own glob does not follow them.
function filesUnder(directory: string, tail: string): string[] {
    const found: string[] = [];
    const visit = (current: string): void => {
        for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
            if (entry.name.startsWith(".")) {
                continue;
            }
            const file = path.join(current, entry.name);
            if (
                entry.isDirectory() ||
                (entry.isSymbolicLink() &&
                    fs.statSync(file, { throwIfNoEntry: false })?.isDirectory() === true)
            ) {
                visit(file);
                continue;
            }
            const relative = path.relative(directory, file);
            if (relative === tail || relative.endsWith(`/${tail}`)) {
                found.push(file);
            }
        }
    };
    visit(directory);
    return found;
}

/// The directories of libc++'s module sources for a target in the
/// toolchain's repositories.
export function stdModuleSources(outputBase: string, triple: string): string[] {
    const external = path.join(outputBase, "external");
    return fs
        .readdirSync(external)
        .filter((name) => name.startsWith("xclang++xclang+xclang_"))
        .flatMap((name) => filesUnder(path.join(external, name), "share/libc++/v1/std.cppm"))
        .filter((file) => file.includes(`/${triple}/`))
        .map((file) => path.dirname(file));
}

/// What `clice index --json` reports of the units that compiled with errors.
export interface IndexReport {
    compiledWithErrors: {
        unit: string;
        errors: { file: string; line: number; message: string }[];
    }[];
}

/// Each unit that compiled with errors, by its first one.
export function compileFailures(name: string, report: IndexReport): string[] {
    return report.compiledWithErrors.flatMap((unit) =>
        unit.errors
            .slice(0, 1)
            .map(
                (error) =>
                    `${name}: ${unit.unit} does not compile: ${error.file}:${error.line}: ${error.message}`,
            ),
    );
}

/// The merge file of `clice modularize merge`, beside the configurations'
/// directories.
export function mergeFile(configurations: readonly Configuration[]): string {
    return JSON.stringify(
        {
            configurations: configurations.map(({ name, condition }) => ({
                name,
                condition,
                out: name,
            })),
        },
        null,
        2,
    );
}

function fail(message: string): never {
    console.error(message);
    process.exit(1);
}

/// Runs a program to its end and returns what it printed on a piped stdout.
/// One that does not start or fails stops the script, with what it printed
/// on the streams piped.
function run(program: string, args: readonly string[], stdio: StdioOptions, cwd?: string): string {
    const result = spawnSync(program, args, { cwd, stdio, encoding: "utf8", maxBuffer: Infinity });
    const command = [program, ...args].join(" ");
    if (result.error !== undefined) {
        fail(`${command}: ${result.error.message}`);
    }
    if (result.status !== 0) {
        for (const printed of result.output) {
            if (printed !== null) {
                process.stderr.write(printed);
            }
        }
        fail(`${command} exited with ${result.status ?? result.signal ?? "no status"}`);
    }
    return result.stdout;
}

function bazel(args: readonly string[], stdio: StdioOptions): string {
    return run("npx", ["bazel", ...args], stdio, REPO_ROOT);
}

function generate(configuration: Configuration, clice: string): void {
    const { name, triple } = configuration;
    // A configuration's directory exists only after its last generation
    // went through: merge takes whatever is there.
    const out = path.join(OUT, name);
    const work = path.join(OUT, `${name}.index`);
    fs.rmSync(out, { recursive: true, force: true });
    fs.rmSync(work, { recursive: true, force: true });
    fs.mkdirSync(work, { recursive: true });
    const events = path.join(work, "events.json");
    bazel(buildArguments(configuration, events), "inherit");
    const executionRoot = bazel(["info", "execution_root"], "pipe").trim();
    const outputBase = bazel(["info", "output_base"], "pipe").trim();
    const fragments = compdbFragments(fs.readFileSync(events, "utf8"));
    const [fragment] = fragments;
    if (fragment === undefined || fragments.length > 1) {
        fail(`${name}: the compile command of //modules:demand: ${JSON.stringify(fragments)}`);
    }
    fs.writeFileSync(
        path.join(work, "compile_commands.json"),
        compileCommands(fs.readFileSync(decodeURIComponent(fragment), "utf8"), executionRoot),
    );
    fs.writeFileSync(
        path.join(work, "clice.toml"),
        '[[rules]]\ncompile_commands = ["compile_commands.json"]\n',
    );
    fs.writeFileSync(
        path.join(work, "partition.json"),
        JSON.stringify(partition(outputBase, clice), null, 2),
    );
    const sources = stdModuleSources(outputBase, triple);
    const [std] = sources;
    if (std === undefined || sources.length > 1) {
        fail(`${name}: libc++'s module sources for ${triple}: ${JSON.stringify(sources)}`);
    }

    const report = JSON.parse(
        run(clice, ["index", "--workspace", work, "--json"], ["inherit", "pipe", "inherit"]),
    ) as IndexReport;
    // A unit that does not compile leaves out of the modules what it fails
    // to declare (a clice without its resource headers fails them all).
    const failures = compileFailures(name, report);
    if (failures.length > 0) {
        fail(failures.join("\n"));
    }
    const plan = JSON.parse(
        run(
            clice,
            [
                "modularize",
                "--workspace",
                work,
                "--scope",
                "/**",
                "--partition",
                path.join(work, "partition.json"),
                "--std",
                std,
                "--out",
                path.join(work, "wrapping"),
                "--no-mirrors",
            ],
            "pipe",
        ),
    ) as { wrapping: { warnings: string[] } };
    // A header the partition leaves to the program, or modules sharing a
    // TU-local entity: the partition needs fixing.
    const { warnings } = plan.wrapping;
    if (warnings.length > 0) {
        fail(warnings.map((warning) => `${name}: ${warning}`).join("\n"));
    }
    fs.renameSync(path.join(work, "wrapping"), out);
}

function merge(clice: string): void {
    const missing = CONFIGURATIONS.filter(
        ({ name }) =>
            fs.statSync(path.join(OUT, name), { throwIfNoEntry: false })?.isDirectory() !== true,
    ).map(({ name }) => name);
    if (missing.length > 0) {
        fail(`not generated: ${missing.join(", ")}`);
    }
    const file = path.join(OUT, "merge.json");
    fs.writeFileSync(file, mergeFile(CONFIGURATIONS));
    run(
        clice,
        ["modularize", "merge", file, "--out", path.join(REPO_ROOT, "modules"), "--no-mirrors"],
        ["inherit", "pipe", "inherit"],
    );
}

const USAGE =
    "usage: node tools/modules/modules.ts [--clice <path>] generate [<configuration>...]\n" +
    "       node tools/modules/modules.ts [--clice <path>] merge";

function usageError(message: string): never {
    console.error(`${USAGE}\nerror: ${message}`);
    process.exit(2);
}

function parseCommandLine() {
    try {
        return parseArgs({
            options: {
                clice: { type: "string" },
                help: { type: "boolean", short: "h" },
            },
            allowPositionals: true,
        });
    } catch (error) {
        usageError(error instanceof Error ? error.message : String(error));
    }
}

function main(): void {
    const { values, positionals } = parseCommandLine();
    const names = CONFIGURATIONS.map(({ name }) => name).join(", ");
    if (values.help === true) {
        console.log(`${USAGE}\n\nconfigurations: ${names}`);
        return;
    }
    const clice = path.resolve(
        values.clice ?? path.join(REPO_ROOT, "build/RelWithDebInfo/bin/bin/clice"),
    );
    const [command, ...rest] = positionals;
    if (command === "generate") {
        const chosen = rest.map(
            (name) =>
                CONFIGURATIONS.find((each) => each.name === name) ??
                usageError(`invalid configuration: ${name} (choose from ${names})`),
        );
        if (process.platform !== "linux" || os.machine() !== "x86_64") {
            fail("modules.ts generates on x64 Linux");
        }
        for (const configuration of chosen.length === 0 ? CONFIGURATIONS : chosen) {
            generate(configuration, clice);
        }
    } else if (command === "merge" && rest.length === 0) {
        merge(clice);
    } else if (command === "merge") {
        usageError(`unrecognized arguments: ${rest.join(" ")}`);
    } else {
        usageError(
            command === undefined
                ? "a command is required: generate or merge"
                : `invalid command: ${command}`,
        );
    }
}

if (import.meta.main) {
    main();
}
