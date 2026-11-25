/// Holds every use of clang's offsets in src/ to CompilationUnit: a file's
/// offsets in clang count the byte order mark it starts with, clice's do
/// not, and the unit converts between them (decompose, create_location).
/// A call that decomposes a location, composes one from an offset, takes a
/// file's start or a location's raw encoding, or reads clang's lines and
/// columns is allowed in the unit and the token map it builds, and
/// elsewhere where the comment block right above the statement it is in
/// starts a line with `// Clang's offsets: <why>`: offsets that meet only
/// clang's own buffers, or a preprocessor of its own that converts them.
///
/// It needs no build.
///
/// Usage:
///   node tools/precheck/offsets.ts

import * as fs from "node:fs";
import * as path from "node:path";
import { REPO_ROOT } from "../repo.ts";

const SOURCES = new Set([".cpp", ".cppm", ".h", ".cc"]);

/// The unit, which converts, and the token map, which counts from where it
/// says the focus file's text starts.
const OWNERS = new Set([
    "src/compile/compilation_unit.cpp",
    "src/compile/tokens.cpp",
    "src/compile/tokens.cppm",
]);

const OFFSETS =
    /\b(?:getDecomposed(?:Expansion|Spelling)?Loc|getFileOffset|getComposedLoc|getLocForStartOfFile|getRawEncoding|translate(?:File)?LineCol|get(?:Spelling|Expansion|Presumed)?(?:Line|Column)Number)\(/;
const LABEL = /^\s*\/\/+\s*Clang's offsets: \S/;
const COMMENT = /^\s*\/\//;
const STATEMENT_BOUNDARY = /[;{}]\s*(?:\/\/.*)?$|^\s*\/\//;

function labelled(lines: readonly string[], index: number): boolean {
    // Back to the first line of the statement: the one after a line that
    // ends one, opens a block or is a comment.
    let first = index;
    while (first > 0 && !STATEMENT_BOUNDARY.test(lines[first - 1] ?? "")) {
        first -= 1;
    }
    for (let above = first - 1; above >= 0 && COMMENT.test(lines[above] ?? ""); above -= 1) {
        if (LABEL.test(lines[above] ?? "")) {
            return true;
        }
    }
    return false;
}

/// The uses of clang's offsets in a source that neither its owners nor a
/// label allow, each with its line.
export function unlabelledOffsets(file: string, text: string): string[] {
    if (OWNERS.has(file)) {
        return [];
    }
    const lines = text.split(/\r\n?|\n/);
    return lines.flatMap((line, index) =>
        COMMENT.test(line) || !OFFSETS.test(line) || labelled(lines, index)
            ? []
            : [`${file}:${index + 1}: ${line.trim()}`],
    );
}

/// The files under a directory, sorted.
function filesUnder(directory: string): string[] {
    return fs
        .readdirSync(directory, { recursive: true, encoding: "utf8" })
        .sort()
        .map((file) => path.join(directory, file));
}

function main(): void {
    const failures = filesUnder(path.join(REPO_ROOT, "src"))
        .filter((file) => SOURCES.has(path.extname(file)))
        .flatMap((file) =>
            unlabelledOffsets(
                path.relative(REPO_ROOT, file).split(path.sep).join("/"),
                fs.readFileSync(file, "utf8"),
            ),
        );
    for (const failure of failures) {
        console.log(failure);
    }
    if (failures.length > 0) {
        console.error(
            "\nA file's offsets in clang count its byte order mark, clice's do not: take " +
                "them from CompilationUnit (decompose, written_at, create_location). Offsets " +
                "that meet only clang's own buffers follow a comment block with a line " +
                "`// Clang's offsets: <why>` (see tools/precheck/offsets.ts).",
        );
        process.exitCode = 1;
    }
}

if (import.meta.main) {
    main();
}
