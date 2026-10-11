/// Shared by the one-off migration checks of S-TOKENS (semantic_tokens.ts,
/// folding_range.ts, selection_range.ts, document_links.ts): an old snap
/// corpus read back into facts rendered the way the new output names them,
/// the new snap.toml files of a tests-next topic read into the same facts,
/// and the tally of what is missing, what is explained and how many things
/// each side asks.

import * as fs from "node:fs";
import * as path from "node:path";
import { parseAnnotations } from "../snap/annotation.ts";
import { snapCorpora, type SnapCorpus, type SnapFixture } from "../snap/corpus.ts";
import { parseSnap } from "../snap/snapshot.ts";
import { CASES_DIR } from "../testing/cases/project.ts";
import { renderLocation, SourceText, type Range } from "../testing/render/position.ts";
import { readToml } from "../testing/render/value.ts";

export function corpus(feature: string): SnapCorpus {
    const found = snapCorpora().find((c) => c.feature === feature);
    if (found === undefined) {
        throw new Error(`no snap corpus ${feature}`);
    }
    return found;
}

/// An old snapshot's body by the file each `--- file` head names, the entry
/// file under its own name; `variant` picks a separate fixture's path.
export function oldBody(
    c: SnapCorpus,
    fixture: SnapFixture,
    variant: "" | "inspect" | "server" = "",
): Map<string, string[]> {
    const base = path.join(c.corpus, fixture.rel.replace(/\.cpp$/, ""));
    const file = variant === "" ? `${base}.snap.yml` : `${base}.${variant}.snap.yml`;
    const snap = parseSnap(fs.readFileSync(file, "utf8"));
    if (snap === null) {
        throw new Error(`${file}: no front matter`);
    }
    const out = new Map<string, string[]>();
    let current = path.posix.basename(fixture.rel);
    for (const line of snap.body.split("\n")) {
        const head = /^--- (.+)$/.exec(line);
        if (head !== null) {
            current = head[1] ?? "";
            continue;
        }
        if (line.trim() === "") {
            continue;
        }
        out.set(current, [...(out.get(current) ?? []), line]);
    }
    return out;
}

/// A fixture file's text as both old paths compiled it, markers stripped.
export function oldSource(fixture: SnapFixture, name?: string): SourceText {
    const file =
        name === undefined
            ? fixture.files.find((f) => f.rel === fixture.rel)
            : fixture.files.find((f) => f.rel.endsWith(`/${name}`) || f.rel === name);
    if (file === undefined) {
        throw new Error(`${fixture.rel}: no file ${name ?? "entry"}`);
    }
    return new SourceText(path.posix.basename(file.rel), file.source.content);
}

/// `L:C` as an LSP position.
export function position(text: string): { line: number; character: number } {
    const [line, character] = text.split(":").map(Number);
    return { line: line ?? 0, character: character ?? 0 };
}

/// `L:C-L:C` as an LSP range.
export function range(text: string): Range {
    const [start, end] = text.split("-");
    return { start: position(start ?? ""), end: position(end ?? "") };
}

/// A rendering without the offset that only tells repeated lines apart:
/// line numbers differ between the old fixture and the new case.
export function bare(rendered: string): string {
    return rendered.replace(/ @[+-]?\d+$/, "");
}

/// How the new output names `r` in `source`, offset dropped.
export function render(source: SourceText, r: Range): string {
    return bare(
        renderLocation(
            { path: source.path, range: r },
            { file: () => source, query: { path: source.path } },
        ),
    );
}

/// The blocks of a case's snap.toml, by test and label; a separate test's
/// `inspect` and `server` halves come back as their own tests.
export function newBlocks(caseId: string): Map<string, Map<string, Record<string, unknown>>> {
    const file = path.join(CASES_DIR, caseId, "snap.toml");
    if (!fs.existsSync(file)) {
        throw new Error(`${caseId}: no snap.toml`);
    }
    const out = new Map<string, Map<string, Record<string, unknown>>>();
    for (const [test, blocks] of Object.entries(readToml(fs.readFileSync(file, "utf8")))) {
        const table = blocks as Record<string, Record<string, unknown>>;
        const keys = Object.keys(table);
        const apart = keys.length > 0 && keys.every((key) => key === "inspect" || key === "server");
        // A block's known-wrong reason is no block of its own.
        const blocksOf = (part: object): Map<string, Record<string, unknown>> =>
            new Map(
                Object.entries(part as Record<string, Record<string, unknown>>).filter(
                    ([label]) => label !== "wrong",
                ),
            );
        if (apart) {
            for (const key of keys) {
                out.set(`${test}.${key}`, blocksOf(table[key] ?? {}));
            }
        } else {
            out.set(test, blocksOf(table));
        }
    }
    return out;
}

/// A case file's text with markers stripped, and where each named marker
/// sits as a UTF-16 position.
export function caseSource(
    caseId: string,
    file = "main.cpp",
): { source: SourceText; markers: Map<string, { line: number; character: number }> } {
    const annotated = parseAnnotations(fs.readFileSync(path.join(CASES_DIR, caseId, file), "utf8"));
    const source = new SourceText(file, annotated.content);
    const bytes = Buffer.from(annotated.content);
    const markers = new Map<string, { line: number; character: number }>();
    for (const [name, offset] of annotated.offsets) {
        markers.set(name, source.positionAt(bytes.subarray(0, offset).toString().length));
    }
    return { source, markers };
}

/// The clients an answers test of `caseId` runs on, from the script it
/// inherits: one where it names a single path, else both.
export function clients(caseId: string): number {
    let dir = path.join(CASES_DIR, caseId);
    while (dir.startsWith(CASES_DIR) && dir !== CASES_DIR) {
        const script = path.join(dir, "test.ts");
        if (fs.existsSync(script)) {
            const text = fs.readFileSync(script, "utf8");
            return /paths:\s*\{\s*(server|inspect):/.test(text) ? 1 : 2;
        }
        dir = path.dirname(dir);
    }
    throw new Error(`${caseId}: no test.ts`);
}

/// The things a case's output asks: every answers block once per client, a
/// separate test's halves once each, every scenario block once.
export function asked(caseId: string): number {
    let total = 0;
    const each = clients(caseId);
    for (const [test, blocks] of newBlocks(caseId)) {
        const answers = test === "answers" || test.startsWith("answers.");
        const apart = test.endsWith(".inspect") || test.endsWith(".server");
        const named = !answers && !/^test(\.|$)/.test(test);
        // A named test is an answers test when the case's script says so.
        const perBlock = answers && !apart ? each : named ? namedClients(caseId, test) : 1;
        total += blocks.size * perBlock;
    }
    return total;
}

function namedClients(caseId: string, test: string): number {
    let dir = path.join(CASES_DIR, caseId);
    while (dir.startsWith(CASES_DIR) && dir !== CASES_DIR) {
        const script = path.join(dir, "test.ts");
        if (fs.existsSync(script)) {
            const text = fs.readFileSync(script, "utf8");
            return new RegExp(`answers\\(\\s*"${test}"`).test(text) ? clients(caseId) : 1;
        }
        dir = path.dirname(dir);
    }
    return 1;
}

/// Every case of a topic, by id.
export function topicCases(topic: string): string[] {
    const out: string[] = [];
    for (const subtopic of fs.readdirSync(path.join(CASES_DIR, topic), { withFileTypes: true })) {
        if (!subtopic.isDirectory()) {
            continue;
        }
        for (const entry of fs.readdirSync(path.join(CASES_DIR, topic, subtopic.name))) {
            if (fs.existsSync(path.join(CASES_DIR, topic, subtopic.name, entry, "snap.toml"))) {
                out.push(`${topic}/${subtopic.name}/${entry}`);
            }
        }
    }
    return out.sort();
}

/// Multiset difference: the facts of `old` that `found` lacks.
export function missing(old: readonly string[], found: readonly string[]): string[] {
    const left = new Map<string, number>();
    for (const fact of found) {
        left.set(fact, (left.get(fact) ?? 0) + 1);
    }
    const out: string[] = [];
    for (const fact of old) {
        const count = left.get(fact) ?? 0;
        if (count > 0) {
            left.set(fact, count - 1);
        } else {
            out.push(fact);
        }
    }
    return out;
}

/// What a check found: facts missing with no reason, facts explained, and
/// the things each side asks.
export class Tally {
    readonly unexplained: string[] = [];
    readonly explained: string[] = [];
    oldAsked = 0;
    newAsked = 0;

    /// Record the facts of `fixture` the new output lacks; `explain` gives
    /// the reason a missing fact was changed on purpose, or undefined.
    check(
        fixture: string,
        old: readonly string[],
        found: readonly string[],
        explain: (fact: string) => string | undefined,
    ): void {
        for (const fact of missing(old, found)) {
            const reason = explain(fact);
            if (reason === undefined) {
                this.unexplained.push(`${fixture}: ${fact}`);
            } else {
                this.explained.push(`${fixture}: ${fact}  [${reason}]`);
            }
        }
    }

    print(feature: string): void {
        console.log(`${feature}: asked ${this.oldAsked} before, ${this.newAsked} now`);
        if (this.explained.length > 0) {
            console.log(`explained (${this.explained.length}):\n  ${this.explained.join("\n  ")}`);
        }
        if (this.unexplained.length > 0) {
            console.log(
                `UNEXPLAINED (${this.unexplained.length}):\n  ${this.unexplained.join("\n  ")}`,
            );
            process.exitCode = 1;
        } else {
            console.log("no unexplained differences");
        }
    }
}
