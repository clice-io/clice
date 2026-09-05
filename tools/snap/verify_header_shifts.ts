import * as fs from "node:fs";
import * as path from "node:path";
import { execFileSync } from "node:child_process";
import { REPO_ROOT } from "../compile_commands.ts";
import { scanFixtureHeader } from "./corpus.ts";

function fromGit(ref: string, file: string): string {
    return execFileSync("git", ["show", `${ref}:${file}`], {
        cwd: REPO_ROOT,
        encoding: "utf8",
    });
}

function exampleStart(content: string): number {
    const header = scanFixtureHeader(content);
    if (header.headings.length === 0) {
        return 0;
    }
    let line = header.bodyStart;
    while (line < header.lines.length && (header.lines[line] ?? "").trim() === "") {
        line += 1;
    }
    if ((header.lines[line] ?? "").trimStart().startsWith("// snap:")) {
        while ((header.lines[line] ?? "").trimStart().startsWith("//")) {
            line += 1;
        }
        while (line < header.lines.length && (header.lines[line] ?? "").trim() === "") {
            line += 1;
        }
    }
    return line;
}

function sourceOf(snapshot: string): string {
    return snapshot
        .replace(/\.(?:inspect|server)\.snap\.yml$/, ".cpp")
        .replace(/\.snap\.yml$/, ".cpp");
}

const POSITION_RE = /\b(\d+):(\d+)\b/g;

function positions(text: string): [number, number][] {
    return [...text.matchAll(POSITION_RE)].map((match) => [
        Number.parseInt(match[1] ?? "", 10),
        Number.parseInt(match[2] ?? "", 10),
    ]);
}

function withoutHeaderTokens(text: string, start: number): { text: string; removed: number } {
    let removed = 0;
    const lines = text.split("\n").filter((line) => {
        const match = /^- \{ loc: "(\d+):\d+".* kind: comment/.exec(line);
        if (match && Number.parseInt(match[1] ?? "", 10) < start) {
            removed += 1;
            return false;
        }
        return true;
    });
    return { text: lines.join("\n"), removed };
}

function verify(
    snapshot: string,
    oldSnapshot: string,
    ref: string,
): { delta: number; shifted: number; unchanged: number; headerTokens: number } {
    const source = sourceOf(snapshot);
    const oldSourcePath = sourceOf(oldSnapshot);
    const oldSource = fromGit(ref, oldSourcePath);
    const newSource = fs.readFileSync(path.join(REPO_ROOT, source), "utf8");
    const oldStart = exampleStart(oldSource);
    const newStart = exampleStart(newSource);
    const delta = newStart - oldStart;
    let oldSnapshotText = fromGit(ref, oldSnapshot);
    let newSnapshot = fs.readFileSync(path.join(REPO_ROOT, snapshot), "utf8");
    const oldRelative = oldSourcePath.split("/").slice(3).join("/");
    const newRelative = source.split("/").slice(3).join("/");
    oldSnapshotText = oldSnapshotText.replaceAll(oldRelative, newRelative);
    let headerTokens = 0;
    if (
        oldSnapshotText.replaceAll(POSITION_RE, "L:C") !==
        newSnapshot.replaceAll(POSITION_RE, "L:C")
    ) {
        const oldWithoutHeader = withoutHeaderTokens(oldSnapshotText, oldStart);
        const newWithoutHeader = withoutHeaderTokens(newSnapshot, newStart);
        oldSnapshotText = oldWithoutHeader.text;
        newSnapshot = newWithoutHeader.text;
        headerTokens = oldWithoutHeader.removed + newWithoutHeader.removed;
        if (
            oldSnapshotText.replaceAll(POSITION_RE, "L:C") !==
            newSnapshot.replaceAll(POSITION_RE, "L:C")
        ) {
            throw new Error(
                `${snapshot}: content changed beyond source positions and header tokens`,
            );
        }
    }
    const before = positions(oldSnapshotText);
    const after = positions(newSnapshot);
    let shifted = 0;
    let unchanged = 0;
    for (let i = 0; i < before.length; i++) {
        const oldPosition = before[i];
        const newPosition = after[i];
        if (oldPosition?.[1] !== newPosition?.[1]) {
            throw new Error(`${snapshot}: column changed at position ${i + 1}`);
        }
        if (newPosition?.[0] === (oldPosition?.[0] ?? 0) + delta) {
            shifted += 1;
        } else if (newPosition?.[0] === oldPosition?.[0]) {
            // Multi-file fixtures also carry locations from markerless sibling
            // files, whose header delta is zero.
            unchanged += 1;
        } else {
            throw new Error(
                `${snapshot}: line ${i + 1} changed ${oldPosition?.[0]} -> ${newPosition?.[0]}, expected delta ${delta}`,
            );
        }
    }
    return { delta, shifted, unchanged, headerTokens };
}

const ref = process.argv[2];
const corpus = process.argv[3];
const ignored = new Set(process.argv.slice(4));
if (!ref || !corpus) {
    console.error("usage: verify_header_shifts.ts <ref> <corpus> [ignored-source ...]");
    process.exit(2);
}
const root = `tests/snap/${corpus}`;
const nameStatus = execFileSync("git", ["diff", "--name-status", "-M", ref, "--", root], {
    cwd: REPO_ROOT,
    encoding: "utf8",
})
    .trim()
    .split("\n")
    .filter(Boolean);
const sourceRenames = new Map<string, string>();
for (const line of nameStatus) {
    const fields = line.split("\t");
    if ((fields[0] ?? "").startsWith("R") && (fields[2] ?? "").endsWith(".cpp")) {
        sourceRenames.set(fields[2] ?? "", fields[1] ?? "");
    }
}
function oldSnapshotPath(newPath: string, oldSource: string): string {
    const mode = /\.(inspect|server)\.snap\.yml$/.exec(newPath)?.[1];
    return oldSource.replace(/\.cpp$/, `${mode === undefined ? "" : `.${mode}`}\.snap.yml`);
}
const changed = execFileSync(
    "git",
    ["diff", "--name-only", "--diff-filter=AMR", "-M", ref, "--", root],
    {
        cwd: REPO_ROOT,
        encoding: "utf8",
    },
)
    .trim()
    .split("\n")
    .filter((file) => file.endsWith(".snap.yml"))
    .map((newPath): { oldPath: string; newPath: string } => {
        const newSource = sourceOf(newPath);
        const oldSource = sourceRenames.get(newSource) ?? newSource;
        return { oldPath: oldSnapshotPath(newPath, oldSource), newPath };
    })
    .filter(
        ({ oldPath, newPath }) =>
            !ignored.has(sourceOf(oldPath)) && !ignored.has(sourceOf(newPath)),
    );
let shiftedFiles = 0;
let failures = 0;
for (const { oldPath, newPath } of changed) {
    try {
        const result = verify(newPath, oldPath, ref);
        if (result.delta !== 0) {
            shiftedFiles += 1;
        }
        const display = oldPath === newPath ? newPath : `${oldPath} -> ${newPath}`;
        console.log(
            `${display}: delta ${result.delta}, ${result.shifted} shifted positions, ${result.unchanged} sibling positions, ${result.headerTokens} header tokens excluded`,
        );
    } catch (error) {
        failures += 1;
        console.error(error instanceof Error ? error.message : String(error));
    }
}
console.log(`${corpus}: verified ${changed.length} snapshots (${shiftedFiles} shifted files)`);
process.exitCode = failures === 0 ? 0 : 1;
