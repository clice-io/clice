/// Symbolize a clice crash log against a release's symbol file (the release
/// skill, "Crash log support").
///
/// Each frame of a crash names its module, where the module is loaded and
/// the offset in it ("0x..., /usr/bin/clice(0x...) + 0x... byte(s)"): the
/// offset is the address the symbols of an ELF or Mach-O file count, and for
/// a Windows module (.exe, .dll) the address from the executable's preferred
/// image base (--image-base). Every frame but the one a signal or an
/// exception interrupted (its line ends with " (interrupted)") is a return
/// address, whose call is the byte before it. Older logs number their frames
/// and record the executable's load address ("main executable base: 0x...")
/// instead, which is subtracted.
///
/// Usage:
///   node tools/release/symbolize.ts <crash log | -> --symbols <file> [options]
///
/// Accepts either the released GSYM symbol file (resolved with
/// llvm-gsymutil) or a full DWARF file / unstripped binary (resolved with
/// llvm-symbolizer). For a macOS dSYM pass the inner DWARF file
/// (clice.dSYM/Contents/Resources/DWARF/clice), not the bundle directory.
/// GSYM output keeps names mangled; pipe it through llvm-cxxfilt to
/// demangle.

import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { buffer } from "node:stream/consumers";
import { parseArgs } from "node:util";

const USAGE = `usage: node tools/release/symbolize.ts <log> --symbols <file> [options]

  <log>                   the crash log, or - for stdin
  --symbols <file>        clice.gsym, an unstripped clice, or a dSYM's inner DWARF file
  --module <name>         the frames' module treated as the main executable (default: clice)
  --image-base <address>  a Windows module's preferred image base (default: 0x140000000,
                          lld's for 64-bit EXEs)
  --symbolizer <program>  (default: llvm-symbolizer)
  --gsymutil <program>    (default: llvm-gsymutil)`;

/// "0x00007FF6A1B2C3D4, C:\...\clice.exe(0x00007FF6A1B20000) + 0x2C3D4 byte(s)",
/// on every OS; " (interrupted)" after the frame a signal or an exception
/// interrupted. LLVM's Windows trace, which older Windows logs hold, has the
/// same lines without it.
const MODULE_FRAME =
    /^\s*0x[0-9a-fA-F]+, (.+?)\(0x[0-9a-fA-F]+\) \+ 0x([0-9a-fA-F]+) byte\(s\)( \(interrupted\))?/;
/// Older logs: "0  clice 0x00005ea84c157818", the un-symbolized dump LLVM
/// writes when the crashing machine has no llvm-symbolizer, rebased by the
/// "main executable base" line before it. Already-symbolized "#0 0x..."
/// frames do not match and pass through untouched.
const FRAME = /^\s*(\d+)\s+(\S+)\s+0x([0-9a-fA-F]+)/;
const BASE = /main executable base: 0x([0-9a-fA-F]+)/;

export interface FrameOptions {
    /// The frames' module treated as the main executable.
    module: string;
    /// A Windows module's preferred image base.
    imageBase: bigint;
}

export interface Tools {
    symbolizer: string;
    gsymutil: string;
}

export const DEFAULTS: FrameOptions & Tools = {
    module: "clice",
    imageBase: 0x140000000n,
    symbolizer: "llvm-symbolizer",
    gsymutil: "llvm-gsymutil",
};

/// A frame's offset in the main executable resolved: its function, then the
/// functions inlined into it, a line each; undefined where the tool has no
/// answer.
export type Resolve = (offset: bigint) => string[] | undefined;

/// The text's lines, split at every line break the Unicode standard names,
/// a break at the end ending the last line.
export function splitLines(text: string): string[] {
    const lines = text.split(/\r\n|[\n\v\f\r\x1c-\x1e\x85\u2028\u2029]/);
    if (lines.at(-1) === "") {
        lines.pop();
    }
    return lines;
}

/// An integer literal: decimal, or 0x, 0o or 0b prefixed, with single `_`s
/// between its digits, a sign and spaces around it.
export function parseInteger(text: string): bigint | undefined {
    const match =
        /^([+-]?)(0[xX](?:_?[0-9a-fA-F])+|0[oO](?:_?[0-7])+|0[bB](?:_?[01])+|[1-9](?:_?\d)*|0(?:_?0)*)$/.exec(
            text.trim(),
        );
    if (match === null) {
        return undefined;
    }
    const [, sign, digits = ""] = match;
    const value = BigInt(digits.replaceAll("_", ""));
    return sign === "-" ? -value : value;
}

export function hex(value: bigint): string {
    return value < 0n ? `-0x${(-value).toString(16)}` : `0x${value.toString(16)}`;
}

/// The log as it is printed symbolized: every frame of the main executable
/// `resolve` answers for replaced by `#<number> <offset> ` and the answer's
/// lines, the inlined functions on lines of their own under it; every other
/// line as it is. An error for a log with neither a frame naming its module
/// nor a load address to rebase by.
export function symbolize(
    text: string,
    options: FrameOptions,
    resolve: Resolve,
): { lines: string[] } | { error: string } {
    const lines = splitLines(text);
    if (!lines.some((line) => BASE.test(line) || MODULE_FRAME.test(line))) {
        return {
            error:
                "no frame naming its module and no 'main executable base' line in the log; " +
                "the crash predates base recording — addresses cannot be rebased",
        };
    }
    const module = options.module.toLowerCase();
    const output: string[] = [];
    // A log can hold several crash sections appended by respawned processes,
    // each with its own ASLR base — track the most recent one while scanning.
    let base: bigint | undefined;
    // Module frames carry no number; they are counted per crash section.
    let moduleFrames = 0;
    for (const line of lines) {
        if (line.includes("=== CRASH STACK TRACE ===")) {
            moduleFrames = 0;
        }
        const baseMatch = BASE.exec(line);
        if (baseMatch !== null) {
            base = BigInt(`0x${baseMatch[1] ?? ""}`);
            moduleFrames = 0;
            output.push(line);
            continue;
        }
        let number: string;
        let offset: bigint;
        const moduleFrame = MODULE_FRAME.exec(line);
        if (moduleFrame !== null) {
            const [, modulePath = "", moduleOffset = "", interrupted] = moduleFrame;
            number = String(moduleFrames);
            moduleFrames += 1;
            const name = path.win32.basename(modulePath).toLowerCase();
            const extension = path.posix.extname(name);
            if (name.slice(0, name.length - extension.length) !== module) {
                output.push(line);
                continue;
            }
            offset = BigInt(`0x${moduleOffset}`);
            if (extension === ".exe" || extension === ".dll") {
                offset += options.imageBase;
            }
            // A return address: its call is the byte before it.
            if (interrupted === undefined) {
                offset -= 1n;
            }
        } else {
            const frame = FRAME.exec(line);
            if (
                frame === null ||
                base === undefined ||
                path.basename(frame[2] ?? "") !== options.module
            ) {
                output.push(line);
                continue;
            }
            number = frame[1] ?? "";
            offset = BigInt(`0x${frame[3] ?? ""}`) - base;
            if (offset < 0n) {
                output.push(line);
                continue;
            }
        }
        const resolved = resolve(offset);
        output.push(
            resolved === undefined ? line : `#${number} ${hex(offset)} ${resolved.join("\n    ")}`,
        );
    }
    return { lines: output };
}

function isFile(file: string): boolean {
    try {
        return fs.statSync(file).isFile();
    } catch {
        return false;
    }
}

/// Whether `program` runs: the file it names with a directory, else one in
/// the current directory (Windows) or on PATH, PATHEXT's extensions tried on
/// Windows.
export function runnable(program: string): boolean {
    const windows = process.platform === "win32";
    const extensions = windows
        ? (process.env["PATHEXT"] ?? ".COM;.EXE;.BAT;.CMD;.VBS;.JS;.WS;.MSC")
              .split(";")
              .filter((extension) => extension !== "")
        : [];
    const names =
        !windows ||
        extensions.some((extension) => program.toLowerCase().endsWith(extension.toLowerCase()))
            ? [program]
            : extensions.map((extension) => program + extension);
    const directories =
        path.basename(program) !== program
            ? [""]
            : [...(windows ? ["."] : []), ...(process.env["PATH"] ?? "").split(path.delimiter)];
    const executable = (file: string): boolean => {
        try {
            fs.accessSync(file, windows ? fs.constants.F_OK : fs.constants.X_OK);
        } catch {
            return false;
        }
        return isFile(file);
    };
    return directories.some((directory) =>
        names.some((name) => executable(path.join(directory, name))),
    );
}

/// What resolves offsets in `symbols`: llvm-gsymutil for a GSYM,
/// llvm-symbolizer for DWARF or an unstripped binary. An error when the tool
/// is not on PATH or the symbol file is missing.
export function resolver(symbols: string, tools: Tools): { resolve: Resolve } | { error: string } {
    const tool = symbols.endsWith(".gsym") ? tools.gsymutil : tools.symbolizer;
    if (!runnable(tool)) {
        return { error: `${tool} not found in PATH` };
    }
    // Fail up front rather than silently printing every frame unresolved.
    if (!isFile(symbols)) {
        return { error: `symbol file not found: ${symbols}` };
    }
    const gsym = tool === tools.gsymutil;
    return {
        resolve: (offset) => {
            const result = spawnSync(
                tool,
                gsym
                    ? ["--address", hex(offset), symbols]
                    : [`--obj=${symbols}`, "-f", "-C", "-p", hex(offset)],
                { encoding: "utf8", maxBuffer: Infinity },
            );
            if (result.error !== undefined || result.status !== 0) {
                return undefined;
            }
            const lines = splitLines(result.stdout).filter(
                (entry) => entry.trim() !== "" && !entry.includes("Looking up"),
            );
            return lines.length > 0 ? lines : undefined;
        },
    };
}

async function main(): Promise<number> {
    let parsed;
    try {
        parsed = parseArgs({
            options: {
                symbols: { type: "string" },
                module: { type: "string", default: DEFAULTS.module },
                "image-base": { type: "string" },
                symbolizer: { type: "string", default: DEFAULTS.symbolizer },
                gsymutil: { type: "string", default: DEFAULTS.gsymutil },
                help: { type: "boolean", short: "h", default: false },
            },
            allowPositionals: true,
        });
    } catch (error) {
        return usageError((error as Error).message);
    }
    const { values, positionals } = parsed;
    if (values.help) {
        console.log(USAGE);
        return 0;
    }
    const [log, ...extra] = positionals;
    if (log === undefined || values.symbols === undefined) {
        const missing = [
            ...(log === undefined ? ["<log>"] : []),
            ...(values.symbols === undefined ? ["--symbols"] : []),
        ];
        return usageError(`the following arguments are required: ${missing.join(", ")}`);
    }
    if (extra.length > 0) {
        return usageError(`unrecognized arguments: ${extra.join(" ")}`);
    }
    const imageBase =
        values["image-base"] === undefined
            ? DEFAULTS.imageBase
            : parseInteger(values["image-base"]);
    if (imageBase === undefined) {
        return usageError(`argument --image-base: invalid value: '${values["image-base"] ?? ""}'`);
    }

    const opened = resolver(values.symbols, values);
    if ("error" in opened) {
        console.error(`error: ${opened.error}`);
        return 1;
    }
    let text: string;
    try {
        text =
            log === "-"
                ? (await buffer(process.stdin)).toString("utf8")
                : fs.readFileSync(log, "utf8");
    } catch (error) {
        console.error(`error: cannot read ${log}: ${(error as Error).message}`);
        return 1;
    }
    const symbolized = symbolize(text, { module: values.module, imageBase }, opened.resolve);
    if ("error" in symbolized) {
        console.error(`error: ${symbolized.error}`);
        return 1;
    }
    for (const line of symbolized.lines) {
        console.log(line);
    }
    return 0;
}

function usageError(message: string): number {
    console.error(USAGE.split("\n")[0]);
    console.error(`error: ${message}`);
    return 2;
}

if (import.meta.main) {
    process.exitCode = await main();
}
