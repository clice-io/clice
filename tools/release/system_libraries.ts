/// Assert that clice's program, as a package holds it once stripped, only
/// depends on system dynamic libraries, and that on Linux and macOS its
/// malloc is mimalloc's. It reads the program before stripping, which drops
/// symbols alone: a static program's symbol table is what tells its malloc.
///
/// It inspects the program's dynamic dependencies and rejects anything that
/// resolves into a conda/pixi env instead of the OS: such a binary loads on
/// the build machine and in every test job, which run inside that env, and
/// fails anywhere else. A static program (the musl targets) loads nothing.
///
/// mimalloc takes malloc over on Linux and macOS (src/mimalloc.cc,
/// --@mimalloc//:override): a dynamic ELF program defines and exports
/// malloc, so that the C library's own calls reach it too, a static one
/// defines it as mimalloc's mi_malloc, and a Mach-O one defines it rather
/// than importing it from libSystem, and registers mimalloc's malloc zone,
/// which serves the system libraries. A release that lost it allocates
/// through the system's malloc and runs as every test does.
///
/// Windows (MinGW) binaries must not import a toolchain's runtime DLLs:
/// libc++, libunwind, winpthreads (xclang links them statically) or the MSVC
/// C++ runtime.
///
/// All branches use LLVM binutils (`llvm-otool` / `llvm-readelf` /
/// `llvm-readobj`), which ship in the build env and read every format on
/// every host.
///
/// Usage:
///   node tools/release/system_libraries.ts <binary>

import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { parseArgs } from "node:util";

const USAGE = "usage: node tools/release/system_libraries.ts <binary>";

/// Linux NEEDED allow list, derived from the real packaged
/// x86_64-unknown-linux-gnu binary. clice links the LLVM package's libc++
/// and libgcc statically, so a portable build only pulls in the glibc
/// runtime pieces. libstdc++.so.6 / libc++.so are left out: either would
/// resolve from the conda RUNPATH (see checkElf).
export const LINUX_NEEDED_ALLOWED: ReadonlySet<string> = new Set([
    "libpthread.so.0",
    "librt.so.1",
    "libdl.so.2",
    "libm.so.6",
    "libc.so.6",
    "ld-linux-x86-64.so.2",
    "ld-linux-aarch64.so.1",
]);

/// MinGW runtime DLLs are lib-prefixed (libc++.dll, libunwind.dll,
/// libwinpthread-1.dll, libgcc_s_seh-1.dll, libstdc++-6.dll), which no
/// Windows system DLL is.
export const PE_FORBIDDEN_PREFIXES = ["lib", "msvcp", "vcruntime"];

/// Path fragments that indicate a dependency or search path pointing into a
/// conda/pixi environment rather than the operating system.
export const CONDA_MARKERS = ["conda", ".pixi", "/envs/"];

/// macOS system library location prefixes. Anything outside these (notably
/// @rpath / @loader_path / absolute conda paths) is a violation.
export const MACOS_SYSTEM_PREFIXES = ["/usr/lib/", "/System/Library/"];
/// clice links the LLVM package's libc++ statically; a dependency on the
/// SDK's libc++ means the link fell back to it (the macOS twin of
/// libstdc++.so.6 on Linux), so it is rejected even though /usr/lib/ is a
/// system prefix.
export const MACOS_FORBIDDEN_DYLIBS = ["libc++.", "libc++abi."];

/// Mach-O's magics: 32/64-bit little/big endian, or a fat/universal archive.
const MACHO_MAGICS = new Set([
    "cffaedfe", // 64-bit LE
    "cefaedfe", // 32-bit LE
    "feedfacf", // 64-bit BE
    "feedface", // 32-bit BE
    "cafebabe", // fat/universal
]);

export type Format = "elf" | "macho" | "pe";

/// The object file format of a file's first four bytes.
export function detectFormat(magic: Uint8Array): Format | undefined {
    const bytes = Buffer.from(magic).toString("hex");
    if (bytes === "7f454c46") {
        return "elf";
    }
    if (MACHO_MAGICS.has(bytes)) {
        return "macho";
    }
    if (bytes.startsWith("4d5a")) {
        return "pe";
    }
    return undefined;
}

/// The violation of a Linux or macOS binary whose malloc is the system's.
export const MALLOC_NOT_OURS =
    "malloc is the system's: mimalloc does not take it over (src/mimalloc.cc)";

export interface Check {
    violations: string[];
    /// What is reported but passes.
    notes: string[];
}

/// Fail closed when a parse yielded nothing: every real executable has at
/// least one dynamic dependency, so parsing none means the tool's output
/// format changed, not that the binary is clean.
function assertParsed(count: number, tool: string, what: string): string[] {
    if (count > 0) {
        return [];
    }
    return [
        `parsed no ${what} from \`${tool}\` output; refusing to vouch for this ` +
            `binary (the tool's output format may have changed)`,
    ];
}

/// An ELF binary's check, from its `llvm-readelf -d` and
/// `llvm-readelf --dyn-syms --wide`.
///
/// The NEEDED allow list is the real gate here: it rejects any C++ runtime
/// (libstdc++/libc++) or other non-glibc dependency, which is what would let
/// a conda-resolved library slip in.
///
/// The conda RUNPATH that Linux conda-clang bakes in via its default config
/// is reported for visibility but is not a hard failure: it is pre-existing,
/// benign (it resolves nothing on a user's machine, and every NEEDED entry
/// is system glibc), and removing it needs a separate toolchain change.
/// macOS, where the config is stripped with --no-default-config, enforces
/// the equivalent LC_RPATH rule strictly (see checkMachO).
export function checkElf(dynamic: string, symbols: string): Check {
    const check: Check = { violations: [], notes: [] };
    let parsed = 0;
    for (const line of dynamic.split("\n")) {
        if (line.includes("(NEEDED)")) {
            const needed = /Shared library: \[([^\]]+)\]/.exec(line)?.[1];
            if (needed === undefined) {
                continue;
            }
            parsed += 1;
            if (!LINUX_NEEDED_ALLOWED.has(needed)) {
                check.violations.push(`unexpected NEEDED dependency: ${needed}`);
            }
        } else if (line.includes("(RUNPATH)") || line.includes("(RPATH)")) {
            const runpath = /\[([^\]]+)\]/.exec(line)?.[1];
            if (runpath !== undefined && CONDA_MARKERS.some((marker) => runpath.includes(marker))) {
                check.notes.push(`RUNPATH references a conda/pixi env: ${runpath}`);
            }
        }
    }
    check.violations.push(...assertParsed(parsed, "llvm-readelf -d", "NEEDED entries"));
    if (!/^\s*\d+:\s+\S+\s+\d+\s+FUNC\s+GLOBAL\s+\S+\s+\d+\s+malloc$/m.test(symbols)) {
        check.violations.push(MALLOC_NOT_OURS);
    }
    return check;
}

/// Whether an ELF program has a dynamic section, from its
/// `llvm-readelf --program-headers --wide`: a static one has no DYNAMIC
/// segment.
export function isDynamic(programHeaders: string): boolean {
    return /^\s+DYNAMIC\s+0x/m.test(programHeaders);
}

/// A static ELF program's check, from its `llvm-readelf --program-headers
/// --wide` and `llvm-readelf --syms --wide`: mimalloc's malloc is an alias of
/// mi_malloc, which musl's is not.
export function checkStaticElf(programHeaders: string, symbols: string): Check {
    const check: Check = { violations: [], notes: [] };
    const loads = [...programHeaders.matchAll(/^\s+LOAD\s+0x/gm)].length;
    check.violations.push(
        ...assertParsed(loads, "llvm-readelf --program-headers", "LOAD segments"),
    );
    const address = (name: string) =>
        new RegExp(
            `^\\s*\\d+:\\s+([0-9a-f]+)\\s+\\d+\\s+FUNC\\s+GLOBAL\\s+\\S+\\s+\\d+\\s+${name}$`,
            "m",
        ).exec(symbols)?.[1];
    const malloc = address("malloc");
    if (malloc === undefined) {
        check.violations.push(
            "no malloc in the symbol table: a stripped program cannot tell whose malloc it is",
        );
    } else if (malloc !== address("mi_malloc")) {
        check.violations.push(MALLOC_NOT_OURS);
    }
    return check;
}

/// A Mach-O binary's check, from its `llvm-otool -L`, `llvm-otool -l` and
/// `llvm-nm -u`.
export function checkMachO(dylibs: string, loadCommands: string, undefinedSymbols: string): Check {
    const check: Check = { violations: [], notes: [] };
    let parsed = 0;
    // The first line of -L output is the file-path header ("<path>:"), not
    // a dependency, so skip it.
    for (const entry of dylibs.split("\n").slice(1)) {
        const line = entry.trim();
        if (line === "" || !line.endsWith(")")) {
            continue;
        }
        const dylib = (line.split(" (", 1)[0] ?? "").trim();
        parsed += 1;
        if (!MACOS_SYSTEM_PREFIXES.some((prefix) => dylib.startsWith(prefix))) {
            check.violations.push(`non-system dylib dependency: ${dylib}`);
        } else if (
            MACOS_FORBIDDEN_DYLIBS.some((prefix) => path.basename(dylib).startsWith(prefix))
        ) {
            check.violations.push(`C++ runtime dylib dependency: ${dylib}`);
        }
    }
    check.violations.push(...assertParsed(parsed, "llvm-otool -L", "dylib dependencies"));

    // LC_RPATH entries must not point into a conda/pixi env.
    for (const [, rpath = ""] of loadCommands.matchAll(
        /cmd LC_RPATH[\s\S]*?path (.+?) \(offset \d+\)$/gm,
    )) {
        if (CONDA_MARKERS.some((marker) => rpath.includes(marker))) {
            check.violations.push(`LC_RPATH points into a conda/pixi env: ${rpath}`);
        }
    }
    const imports = (name: string) =>
        new RegExp(`^\\s*(?:U\\s+)?${name}$`, "m").test(undefinedSymbols);
    if (imports("_malloc") || !imports("_malloc_zone_register")) {
        check.violations.push(MALLOC_NOT_OURS);
    }
    return check;
}

/// A PE (MinGW) binary's check, from its `llvm-readobj --coff-imports`.
export function checkPe(imports: string): Check {
    const check: Check = { violations: [], notes: [] };
    const dlls = [...imports.matchAll(/^\s*Name: (\S+\.dll)\s*$/gim)].map(([, dll = ""]) => dll);
    for (const dll of dlls) {
        if (PE_FORBIDDEN_PREFIXES.some((prefix) => dll.toLowerCase().startsWith(prefix))) {
            check.violations.push(`runtime DLL dependency: ${dll}`);
        }
    }
    check.violations.push(
        ...assertParsed(dlls.length, "llvm-readobj --coff-imports", "imported DLLs"),
    );
    return check;
}

/// A tool's output, its line breaks `\n`.
function run(program: string, ...args: string[]): string {
    const result = spawnSync(program, args, { encoding: "utf8", maxBuffer: Infinity });
    if (result.error !== undefined) {
        throw result.error;
    }
    if (result.status !== 0) {
        throw new Error(
            `${program} failed (exit ${String(result.status ?? result.signal)}):\n${result.stderr}`,
        );
    }
    return result.stdout.replace(/\r\n?/g, "\n");
}

/// A path with the platform's separators and without empty or `.` parts,
/// as the messages name it. `..` stays: what it climbs out of may be a link.
export function tidy(file: string, paths: typeof path.posix = path): string {
    const { root } = paths.parse(file);
    const parts = file
        .slice(root.length)
        .split(paths.sep === "\\" ? /[\\/]/ : "/")
        .filter((part) => part !== "" && part !== ".");
    const tidied = root.replace(/[\\/]/g, paths.sep) + parts.join(paths.sep);
    return tidied === "" ? "." : tidied;
}

function isFile(file: string): boolean {
    try {
        return fs.statSync(file).isFile();
    } catch {
        return false;
    }
}

function magicOf(file: string): Uint8Array {
    const descriptor = fs.openSync(file, "r");
    try {
        const magic = Buffer.alloc(4);
        return magic.subarray(0, fs.readSync(descriptor, magic, 0, 4, 0));
    } finally {
        fs.closeSync(descriptor);
    }
}

function main(): number {
    let parsed;
    try {
        parsed = parseArgs({
            options: { help: { type: "boolean", short: "h", default: false } },
            allowPositionals: true,
        });
    } catch (error) {
        return usageError((error as Error).message);
    }
    if (parsed.values.help) {
        console.log(USAGE);
        return 0;
    }
    const [argument, ...extra] = parsed.positionals;
    if (argument === undefined) {
        return usageError("the following arguments are required: <binary>");
    }
    if (extra.length > 0) {
        return usageError(`unrecognized arguments: ${extra.join(" ")}`);
    }
    const binary = tidy(argument);
    if (!isFile(binary)) {
        console.error(`error: ${binary} is not a file`);
        return 1;
    }

    const magic = magicOf(binary);
    const format = detectFormat(magic);
    if (format === undefined) {
        throw new Error(
            `${binary}: unrecognized object file magic ${Buffer.from(magic).toString("hex")}`,
        );
    }
    const programHeaders =
        format === "elf" ? run("llvm-readelf", "--program-headers", "--wide", binary) : "";
    const check =
        format === "elf"
            ? isDynamic(programHeaders)
                ? checkElf(
                      run("llvm-readelf", "-d", binary),
                      run("llvm-readelf", "--dyn-syms", "--wide", binary),
                  )
                : checkStaticElf(programHeaders, run("llvm-readelf", "--syms", "--wide", binary))
            : format === "macho"
              ? checkMachO(
                    run("llvm-otool", "-L", binary),
                    run("llvm-otool", "-l", binary),
                    run("llvm-nm", "-u", binary),
                )
              : checkPe(run("llvm-readobj", "--coff-imports", binary));
    for (const note of check.notes) {
        console.log(`note: ${note}`);
    }
    if (check.violations.length > 0) {
        console.error(`FAIL: ${binary}:`);
        for (const violation of check.violations) {
            console.error(`  - ${violation}`);
        }
        return 1;
    }
    console.log(
        `OK: ${binary} (${format}) depends only on system libraries` +
            (format === "pe" ? "" : ", and its malloc is mimalloc's"),
    );
    return 0;
}

function usageError(message: string): number {
    console.error(USAGE);
    console.error(`error: ${message}`);
    return 2;
}

if (import.meta.main) {
    process.exitCode = main();
}
