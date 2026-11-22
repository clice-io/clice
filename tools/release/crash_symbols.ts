/// Check that a crash of a release package symbolizes against its symbols.
///
/// What a user's crash report goes through: the package's clice, stripped,
/// crashes (`#pragma clang __debug crash`, which only CLICE_TEST_PRAGMA_CRASH
/// lets through), and symbolize.ts resolves its trace with the symbol
/// package's GSYM. The frame the crash interrupted must name the clang
/// function that crashed, and the frames under it a function of clice's and
/// main.
///
/// Usage:
///   node tools/release/crash_symbols.ts <directory>
///
/// The directory holds what a CI platform that packages uploads: clice.tar.gz
/// or clice.zip, and clice-symbol.tar.xz or clice-symbol.zip.

import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { parseArgs } from "node:util";
import { DEFAULTS, resolver, splitLines, symbolize } from "./symbolize.ts";

const USAGE = "usage: node tools/release/crash_symbols.ts <directory>";

/// A symbolized frame, "#3 0x1401b94a0 0x00000001401b94a0: Lex + 32 @ ...",
/// and the functions inlined into it on the lines under it,
/// "    LexTokenInternal + 35685 @ ... [inlined]". GSYM keeps names mangled,
/// but on Windows those clang declares in an anonymous namespace, which have
/// their bare name.
const SYMBOLIZED_FRAME = /^#(\d+) 0x[0-9a-f]+ 0x[0-9a-fA-F]+: (\S+) \+ \d+ @/;
const INLINED = /^\s+(\S+) \+ \d+ @/;

/// By its number, the functions of each frame of a symbolized log, the
/// inlined ones too.
export function symbolizedFrames(text: string): Map<number, string[]> {
    const frames = new Map<number, string[]>();
    let names: string[] | undefined;
    for (const line of splitLines(text)) {
        const frame = SYMBOLIZED_FRAME.exec(line);
        const inlined = frame === null && names !== undefined ? INLINED.exec(line) : null;
        if (frame !== null) {
            names = [frame[2] ?? ""];
            frames.set(Number(frame[1]), names);
        } else if (inlined !== null) {
            names?.push(inlined[1] ?? "");
        } else {
            names = undefined;
        }
    }
    return frames;
}

/// What the crash's symbolized frames lack of the functions it crashed in.
export function failures(frames: ReadonlyMap<number, readonly string[]>): string[] {
    const under = [...frames].filter(([number]) => number > 0).flatMap(([, names]) => names);
    const found: string[] = [];
    if (
        !(frames.get(0) ?? []).some(
            (name) =>
                name === "HandlePragma" || name.includes("18PragmaDebugHandler12HandlePragma"),
        )
    ) {
        found.push("the interrupted frame is not clang's HandlePragma");
    }
    if (!under.some((name) => name.startsWith("_ZN5clice"))) {
        found.push("no frame under it is clice's");
    }
    if (!under.includes("main")) {
        found.push("no frame under it is main");
    }
    return found;
}

function isFile(file: string): boolean {
    try {
        return fs.statSync(file).isFile();
    } catch {
        return false;
    }
}

/// The archive `stem` of `directory`, a .tar.gz, a .tar.xz or a .zip,
/// unpacked into `into`; false when the directory has none.
function unpack(directory: string, stem: string, into: string): boolean {
    const archive = [`${stem}.tar.gz`, `${stem}.tar.xz`, `${stem}.zip`]
        .map((name) => path.join(directory, name))
        .find(isFile);
    if (archive === undefined) {
        return false;
    }
    fs.mkdirSync(into, { recursive: true });
    // GNU tar reads no zip, and takes `C:` for a remote host: the bsdtar
    // of macOS and of Windows' System32 (which Git Bash's own tar hides on
    // the path) reads both.
    const [program, args] =
        process.platform === "linux" && archive.endsWith(".zip")
            ? ["unzip", ["-q", archive, "-d", into]]
            : [
                  process.platform === "win32"
                      ? path.join(process.env["SystemRoot"] ?? "C:\\Windows", "System32", "tar.exe")
                      : "tar",
                  ["-xf", archive, "-C", into],
              ];
    const result = spawnSync(program, args, { encoding: "utf8" });
    if (result.error !== undefined) {
        throw result.error;
    }
    if (result.status !== 0) {
        throw new Error(`${program} cannot unpack ${archive}:\n${result.stderr}`);
    }
    return true;
}

/// The symbolized crash of the package in `directory`, unpacked under
/// `root`; undefined, its error printed, when a check fails before it.
function crash(directory: string, root: string): string | undefined {
    for (const [stem, into] of [
        ["clice", "package"],
        ["clice-symbol", "symbols"],
    ] as const) {
        if (!unpack(directory, stem, path.join(root, into))) {
            console.error(`error: no ${stem} archive in ${path.normalize(directory)}`);
            return undefined;
        }
    }
    const executable = path.join(
        root,
        "package",
        "clice",
        "bin",
        process.platform === "win32" ? "clice.exe" : "clice",
    );
    const gsym = fs
        .readdirSync(path.join(root, "symbols"), { recursive: true, encoding: "utf8" })
        .find((name) => name.endsWith(".gsym"));
    if (gsym === undefined) {
        console.error("error: no .gsym in the symbol package");
        return undefined;
    }

    fs.writeFileSync(
        path.join(root, "main.cpp"),
        "#pragma clang __debug crash\nint main() { return 0; }\n",
    );
    const crashed = spawnSync(executable, ["inspect", "document-symbols", "main.cpp"], {
        cwd: root,
        env: { ...process.env, CLICE_TEST_PRAGMA_CRASH: "1" },
        encoding: "utf8",
        maxBuffer: Infinity,
        // Hang detection: a clice that hangs instead of crashing.
        timeout: 300_000,
    });
    if (crashed.error !== undefined) {
        throw crashed.error;
    }
    if (!crashed.stderr.includes("=== CRASH STACK TRACE ===")) {
        console.log(crashed.stderr);
        const code =
            crashed.signal === null
                ? String(crashed.status)
                : String(-os.constants.signals[crashed.signal]);
        console.error(`error: the package's clice ended with ${code} and no crash trace`);
        return undefined;
    }

    const opened = resolver(path.join(root, "symbols", gsym), DEFAULTS);
    if ("error" in opened) {
        console.error(`error: ${opened.error}`);
        return undefined;
    }
    const symbolized = symbolize(crashed.stderr, DEFAULTS, opened.resolve);
    if ("error" in symbolized) {
        console.error(`error: ${symbolized.error}`);
        return undefined;
    }
    return symbolized.lines.map((line) => `${line}\n`).join("");
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
    const [directory, ...extra] = parsed.positionals;
    if (directory === undefined) {
        return usageError("the following arguments are required: <directory>");
    }
    if (extra.length > 0) {
        return usageError(`unrecognized arguments: ${extra.join(" ")}`);
    }

    const root = fs.mkdtempSync(path.join(os.tmpdir(), "clice-crash-"));
    let symbolized: string | undefined;
    try {
        symbolized = crash(directory, root);
    } finally {
        fs.rmSync(root, { recursive: true, force: true });
    }
    if (symbolized === undefined) {
        return 1;
    }
    console.log(symbolized);

    const found = failures(symbolizedFrames(symbolized));
    for (const failure of found) {
        console.error(`error: ${failure}`);
    }
    return found.length > 0 ? 1 : 0;
}

function usageError(message: string): number {
    console.error(USAGE);
    console.error(`error: ${message}`);
    return 2;
}

if (import.meta.main) {
    process.exitCode = main();
}
