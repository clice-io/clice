/// Keep docs/en and docs/zh aligned without storing any prose twice.
///
/// Both trees are real sources, edited directly (by hand or by a model).
/// The contract is that a Chinese page is segment-isomorphic to its
/// English counterpart: the same sequence of markdown blocks with the
/// same shape (heading depth, list orderedness, table width), where
/// translatable segments (headings, paragraphs, blockquotes, list items,
/// table rows, index.md's YAML frontmatter) carry the translated text and
/// every other segment (code blocks, HTML comments including GENERATED
/// region markers, ...) is byte-identical — as is any fenced code block
/// nested inside a translatable segment. A table row and a later heading
/// that share their text in en (a capability's status row and its
/// section) share it in zh as well, and the inline literals of a
/// segment — code spans, link and image targets in order, issue
/// references, frontmatter values other than its copy — are identical on
/// both sides. The only stored link between the two sides is
/// docs/meta/translations/<page>.json — one hash pair per translatable
/// segment, in document order:
///
///     { "version": 1,
///       "pairs": [
///         { "kind": "heading", "en": "9f3a…", "zh": "1c2d…" },
///         … ] }
///
/// A pair attests "these two texts were last reviewed as translations of
/// each other". Editing either side changes that side's hash and breaks
/// the pair; `check` then fails until the counterpart is brought up to
/// date (or deliberately confirmed unchanged) and the page is re-attested
/// with `record`. The tool never writes markdown. To see what a drifted
/// segment used to say, use git history of the markdown page.
///
/// Pages under UNTRANSLATED_PREFIXES are excluded from the contract: no
/// docs/zh counterpart, no mapping. The list is empty today; it is the
/// hook for deliberately untranslated content.
///
/// Usage:
///     node tools/docs/translate.ts check    # hard gate: isomorphism + attested pairs
///     node tools/docs/translate.ts report   # translator worklist with segment texts
///     node tools/docs/translate.ts record   # re-attest pages after deliberate edits
///     node tools/docs/translate.ts review [page...]     # model review of zh pages
///
/// `review` re-reads every translatable segment of an existing zh page
/// next to its en counterpart and asks a model for the corrected Chinese —
/// meaning, the wording conventions of the translate-docs skill,
/// naturalness — segment by segment, so no code block ever enters the
/// model's context. It is also how a new or restructured page gets
/// translated: copy the en page over the zh one and review it. The
/// backend runs the codex CLI (GPT-6 astra) with every tool switched
/// off, one call per chunk of segments (a paired row and heading always
/// in the same chunk), `--jobs=N` calls in parallel, `--effort=LEVEL`
/// reasoning and `--fast` for the fast service tier. A reply that breaks
/// a segment's shape, alters an inline literal, or names a row and its
/// heading differently keeps the current Chinese. The pages are rewritten
/// in place; review the diff, then `record`.
///
/// `--en=DIR --zh=DIR --meta=DIR` override the tree roots (for testing).

import { spawn } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import pLimit from "p-limit";
import { REPO_ROOT } from "../compile_commands.ts";
import {
    analyzeSource,
    pairedLabels,
    splitSegments,
    YAML_PROSE_KEYS,
    type Segment,
    type SegmentInfo,
} from "./segment.ts";

const UNTRANSLATED_PREFIXES: string[] = [];

interface Roots {
    en: string;
    zh: string;
    meta: string;
}

interface Pair {
    kind: string;
    en: string;
    zh: string;
}

interface Mapping {
    version: number;
    pairs: Pair[];
}

function isUntranslated(page: string): boolean {
    return UNTRANSLATED_PREFIXES.some((prefix) => page.startsWith(prefix));
}

function listFiles(root: string, extension: string): string[] {
    if (!fs.existsSync(root)) {
        return [];
    }
    const files: string[] = [];
    const walk = (dir: string) => {
        for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
            const full = path.join(dir, entry.name);
            if (entry.isDirectory()) {
                walk(full);
            } else if (entry.name.endsWith(extension)) {
                files.push(path.relative(root, full).split(path.sep).join("/"));
            }
        }
    };
    walk(root);
    return files.sort();
}

function mappingPath(roots: Roots, page: string): string {
    return path.join(roots.meta, page.replace(/\.md$/, ".json"));
}

function loadMapping(roots: Roots, page: string): Mapping | null {
    const file = mappingPath(roots, page);
    if (!fs.existsSync(file)) {
        return null;
    }
    return JSON.parse(fs.readFileSync(file, "utf8")) as Mapping;
}

/// One pair per line so an edited segment shows up as exactly one changed
/// line in the diff.
/// The mapping in the layout prettier gives JSON, so `pixi run format`
/// never rewrites what `record` wrote: one pair per line, except that an
/// array short enough for one line (a page with a single segment) stays on
/// that line, as prettier collapses it.
function serializeMapping(pairs: Pair[]): string {
    const entries = pairs.map(
        (pair) =>
            `{ "kind": ${JSON.stringify(pair.kind)}, ` +
            `"en": ${JSON.stringify(pair.en)}, "zh": ${JSON.stringify(pair.zh)} }`,
    );
    const oneLine = `  "pairs": [${entries.join(", ")}]`;
    const array =
        oneLine.length <= 100
            ? oneLine
            : `  "pairs": [\n${entries.map((entry) => `    ${entry}`).join(",\n")}\n  ]`;
    return `{\n  "version": 1,\n${array}\n}\n`;
}

function zip<A, B>(a: A[], b: B[]): [A, B][] {
    const out: [A, B][] = [];
    const length = Math.min(a.length, b.length);
    for (let i = 0; i < length; i += 1) {
        const left = a.at(i);
        const right = b.at(i);
        if (left !== undefined && right !== undefined) {
            out.push([left, right]);
        }
    }
    return out;
}

interface LabelDiff {
    /// The one name en gives both segments.
    name: string;
    row: SegmentInfo;
    heading: SegmentInfo;
}

interface LiteralDiff {
    left: SegmentInfo;
    right: SegmentInfo;
    /// What zh changed against en, as literalChanges words it.
    changes: string;
}

interface TreeComparison {
    /// Human-readable description of a block-layout divergence, or null
    /// when the two sides are isomorphic.
    structureProblem: string | null;
    /// Segment pairs whose verbatim bytes differ (structure was isomorphic).
    verbatimDiffs: [SegmentInfo, SegmentInfo][];
    /// zh row/heading pairs named alike in en but not in zh (structure was
    /// isomorphic).
    labelDiffs: LabelDiff[];
    /// Segment pairs whose inline literals differ (structure was
    /// isomorphic).
    literalDiffs: LiteralDiff[];
}

function labelProblem(diff: LabelDiff): string {
    return (
        `table row (zh line ${diff.row.line}) and heading (zh line ${diff.heading.line}) ` +
        `are both "${diff.name}" in en but "${diff.row.label ?? ""}" and ` +
        `"${diff.heading.label ?? ""}" in zh — give them one name`
    );
}

/// The literals zh dropped from and added to en's set, or null when the
/// sets agree.
function literalChanges(en: string[], zh: string[]): string | null {
    const dropped = en.filter((literal) => !zh.includes(literal));
    const added = zh.filter((literal) => !en.includes(literal));
    if (dropped.length === 0 && added.length === 0) {
        return null;
    }
    return [
        ...dropped.map((literal) => `dropped ${literal}`),
        ...added.map((literal) => `added ${literal}`),
    ].join(", ");
}

function literalProblem(diff: LiteralDiff): string {
    return `${segmentLabel(diff.left, diff.right)}: inline literals differ — ${diff.changes}`;
}

function describe(info: SegmentInfo | undefined): string {
    return info === undefined ? "ends" : `has ${info.shape} (line ${info.line})`;
}

function sameVerbatim(left: SegmentInfo, right: SegmentInfo): boolean {
    return (
        left.verbatim.length === right.verbatim.length &&
        left.verbatim.every((text, i) => text === right.verbatim.at(i))
    );
}

function compareTrees(en: SegmentInfo[], zh: SegmentInfo[]): TreeComparison {
    const total = Math.max(en.length, zh.length);
    for (let i = 0; i < total; i += 1) {
        const left = en.at(i);
        const right = zh.at(i);
        if (left?.shape !== right?.shape) {
            return {
                structureProblem:
                    `segment layouts diverge at segment ${i + 1}: ` +
                    `en ${describe(left)}, zh ${describe(right)} — ` +
                    `en has ${en.length} segments, zh has ${zh.length}`,
                verbatimDiffs: [],
                labelDiffs: [],
                literalDiffs: [],
            };
        }
    }
    const labelDiffs: LabelDiff[] = [];
    for (const [r, h] of pairedLabels(en)) {
        const row = at(zh, r);
        const heading = at(zh, h);
        if (row.label !== heading.label) {
            labelDiffs.push({ name: at(en, r).label ?? "", row, heading });
        }
    }
    const pairs = zip(en, zh);
    const literalDiffs: LiteralDiff[] = [];
    for (const [left, right] of pairs) {
        const changes = literalChanges(left.literals, right.literals);
        if (changes !== null) {
            literalDiffs.push({ left, right, changes });
        }
    }
    return {
        structureProblem: null,
        verbatimDiffs: pairs.filter(([left, right]) => !sameVerbatim(left, right)),
        labelDiffs,
        literalDiffs,
    };
}

interface PageAnalysis extends TreeComparison {
    page: string;
    en: SegmentInfo[];
    /// null when the Chinese page does not exist.
    zh: SegmentInfo[] | null;
    mapping: Mapping | null;
}

function analyzePage(roots: Roots, page: string): PageAnalysis {
    const en = analyzeSource(fs.readFileSync(path.join(roots.en, page), "utf8"), page);
    const mapping = loadMapping(roots, page);
    const zhFile = path.join(roots.zh, page);
    if (!fs.existsSync(zhFile)) {
        return {
            page,
            en,
            zh: null,
            mapping,
            structureProblem: null,
            verbatimDiffs: [],
            labelDiffs: [],
            literalDiffs: [],
        };
    }
    const zh = analyzeSource(fs.readFileSync(zhFile, "utf8"), `zh/${page}`);
    return { page, en, zh, mapping, ...compareTrees(en, zh) };
}

function translatable(segments: SegmentInfo[]): SegmentInfo[] {
    return segments.filter((segment) => segment.translatable);
}

function segmentLabel(left: SegmentInfo, right: SegmentInfo): string {
    return `segment ${left.index} (${left.kind}, en line ${left.line} / zh line ${right.line})`;
}

function verbatimLabel(left: SegmentInfo, right: SegmentInfo): string {
    return left.translatable
        ? `verbatim block inside ${segmentLabel(left, right)}`
        : `verbatim ${segmentLabel(left, right)}`;
}

function verbatimReason(left: SegmentInfo): string {
    return left.translatable
        ? "nested code or comment must be byte-identical"
        : "verbatim segment must be byte-identical";
}

/// Positional comparison of a recorded pair against the current segments.
/// Only meaningful when the pair count still matches the page.
function driftOf(pair: Pair, left: SegmentInfo, right: SegmentInfo): string | null {
    if (pair.kind !== left.kind) {
        return `recorded as ${pair.kind}, now ${left.kind} — review the page pair, then run record`;
    }
    const enChanged = pair.en !== left.hash;
    const zhChanged = pair.zh !== right.hash;
    if (enChanged && zhChanged) {
        return "both sides changed since last record — verify they still correspond";
    }
    if (enChanged) {
        return "English changed since last record — update the Chinese to match";
    }
    if (zhChanged) {
        return "Chinese changed since last record — confirm it still translates the English";
    }
    return null;
}

interface StrayFiles {
    zhPages: string[];
    mappings: string[];
}

function findStrays(roots: Roots, pages: string[]): StrayFiles {
    // A zh copy of an untranslated page is flagged by the dedicated check
    // with a better message, so it does not count as a stray here.
    const knownPages = new Set(pages);
    const expectedMappings = new Set(
        pages.filter((page) => !isUntranslated(page)).map((page) => page.replace(/\.md$/, ".json")),
    );
    return {
        zhPages: listFiles(roots.zh, ".md").filter((file) => !knownPages.has(file)),
        mappings: listFiles(roots.meta, ".json").filter((file) => !expectedMappings.has(file)),
    };
}

function strayMessages(roots: Roots, pages: string[]): string[] {
    const strays = findStrays(roots, pages);
    return [
        ...strays.zhPages.map(
            (stray) => `zh/${stray}: no English counterpart — remove it or add the English page`,
        ),
        ...strays.mappings.map(
            (stray) => `meta stray ${stray}: no English counterpart — run record to clean up`,
        ),
    ];
}

function check(roots: Roots, pages: string[]): number {
    const problems: string[] = [];
    let attested = 0;
    for (const page of pages) {
        if (isUntranslated(page)) {
            continue;
        }
        const analysis = analyzePage(roots, page);
        if (analysis.zh === null) {
            problems.push(`${page}: Chinese page is missing`);
            continue;
        }
        if (analysis.structureProblem !== null) {
            problems.push(`${page}: ${analysis.structureProblem}`);
            continue;
        }
        for (const [left, right] of analysis.verbatimDiffs) {
            problems.push(
                `${page}: ${verbatimLabel(left, right)} must be byte-identical between en and zh`,
            );
        }
        for (const diff of analysis.labelDiffs) {
            problems.push(`${page}: ${labelProblem(diff)}`);
        }
        for (const diff of analysis.literalDiffs) {
            problems.push(`${page}: ${literalProblem(diff)}`);
        }
        const pairsNow = zip(translatable(analysis.en), translatable(analysis.zh));
        if (analysis.mapping === null) {
            problems.push(`${page}: not recorded — translate, review, then run record`);
            continue;
        }
        if (analysis.mapping.version !== 1) {
            problems.push(`${page}: unsupported mapping version ${analysis.mapping.version}`);
            continue;
        }
        if (analysis.mapping.pairs.length !== pairsNow.length) {
            problems.push(
                `${page}: mapping records ${analysis.mapping.pairs.length} pairs but ` +
                    `the page now has ${pairsNow.length} translatable segments — ` +
                    `review the page pair, then run record`,
            );
            continue;
        }
        for (const [pair, [left, right]] of zip(analysis.mapping.pairs, pairsNow)) {
            const drift = driftOf(pair, left, right);
            if (drift !== null) {
                problems.push(`${page}: ${segmentLabel(left, right)}: ${drift}`);
            } else {
                attested += 1;
            }
        }
    }
    // Untranslated pages must stay untranslated.
    for (const page of pages.filter(isUntranslated)) {
        if (fs.existsSync(path.join(roots.zh, page))) {
            problems.push(`${page}: page is marked untranslated — remove the docs/zh copy`);
        }
        if (fs.existsSync(mappingPath(roots, page))) {
            problems.push(`${page}: page is marked untranslated — remove the mapping file`);
        }
    }
    problems.push(...strayMessages(roots, pages));
    if (problems.length > 0) {
        for (const problem of problems) {
            console.error(problem);
        }
        console.error(`${problems.length} problems`);
        return 1;
    }
    const counted = pages.filter((page) => !isUntranslated(page)).length;
    console.log(`docs/zh matches docs/en (${counted} pages, ${attested} attested segment pairs)`);
    return 0;
}

function quoted(label: string, text: string): string {
    const first = `    ${label} | `;
    const continuation = " ".repeat(label.length + 4) + " | ";
    return text
        .split("\n")
        .map((line, i) => (i === 0 ? first : continuation) + line)
        .join("\n");
}

function reportDrift(left: SegmentInfo, right: SegmentInfo, reason: string): void {
    console.log(`  ${segmentLabel(left, right)} — ${reason}`);
    console.log(quoted("en", left.text));
    console.log(quoted("zh", right.text));
}

function report(roots: Roots, pages: string[]): number {
    let untranslatedPages = 0;
    let driftedSegments = 0;
    let cleanPages = 0;
    for (const page of pages) {
        if (isUntranslated(page)) {
            continue;
        }
        const analysis = analyzePage(roots, page);
        if (analysis.zh === null) {
            console.log(
                `${page}: Chinese page missing ` +
                    `(${translatable(analysis.en).length} segments to translate)`,
            );
            untranslatedPages += 1;
            continue;
        }
        if (analysis.structureProblem !== null) {
            console.log(`${page}: ${analysis.structureProblem}`);
            console.log("    bring the Chinese page to the same block layout, then rerun");
            untranslatedPages += 1;
            continue;
        }
        let pageDrifts = 0;
        const flag = (print: () => void) => {
            if (pageDrifts === 0) {
                console.log(`${page}:`);
            }
            print();
            pageDrifts += 1;
        };
        const drifted = (left: SegmentInfo, right: SegmentInfo, reason: string) => {
            flag(() => {
                reportDrift(left, right, reason);
            });
        };
        for (const [left, right] of analysis.verbatimDiffs) {
            drifted(left, right, verbatimReason(left));
        }
        for (const diff of analysis.labelDiffs) {
            flag(() => {
                console.log(`  ${labelProblem(diff)}`);
            });
        }
        for (const diff of analysis.literalDiffs) {
            drifted(diff.left, diff.right, `inline literals differ — ${diff.changes}`);
        }
        const pairsNow = zip(translatable(analysis.en), translatable(analysis.zh));
        if (analysis.mapping?.version !== 1) {
            const status =
                analysis.mapping === null
                    ? "not recorded"
                    : `unsupported mapping version ${analysis.mapping.version}`;
            console.log(
                `${page}: ${status} (${pairsNow.length} segments) — ` +
                    `review the translation, then run record`,
            );
            untranslatedPages += 1;
            driftedSegments += pageDrifts;
            continue;
        }
        if (analysis.mapping.pairs.length !== pairsNow.length) {
            // Positions shifted (segments were added or removed), so pair
            // them by content instead: anything not covered by a recorded
            // pair needs review.
            const recorded = new Map<string, number>();
            for (const pair of analysis.mapping.pairs) {
                const key = `${pair.en}:${pair.zh}`;
                recorded.set(key, (recorded.get(key) ?? 0) + 1);
            }
            console.log(
                `${page}: layout changed since last record ` +
                    `(${analysis.mapping.pairs.length} pairs recorded, ` +
                    `${pairsNow.length} segments now); segments not covered:`,
            );
            for (const [left, right] of pairsNow) {
                const key = `${left.hash}:${right.hash}`;
                const remaining = recorded.get(key) ?? 0;
                if (remaining > 0) {
                    recorded.set(key, remaining - 1);
                } else {
                    reportDrift(left, right, "no recorded pair");
                    pageDrifts += 1;
                }
            }
            const removed = [...recorded.values()].reduce((sum, count) => sum + count, 0);
            if (removed > 0) {
                console.log(`  ${removed} recorded pairs are no longer on the page`);
                pageDrifts += removed;
            }
            driftedSegments += pageDrifts;
            continue;
        }
        for (const [pair, [left, right]] of zip(analysis.mapping.pairs, pairsNow)) {
            const drift = driftOf(pair, left, right);
            if (drift !== null) {
                drifted(left, right, drift);
            }
        }
        driftedSegments += pageDrifts;
        if (pageDrifts === 0) {
            cleanPages += 1;
        }
    }
    for (const message of strayMessages(roots, pages)) {
        console.log(message);
    }
    console.log(
        `${cleanPages} pages clean, ${untranslatedPages} pages untranslated or ` +
            `unrecorded, ${driftedSegments} drifted segments`,
    );
    return 0;
}

function record(roots: Roots, pages: string[]): number {
    let failed = false;
    for (const page of pages) {
        if (isUntranslated(page)) {
            continue;
        }
        const analysis = analyzePage(roots, page);
        if (analysis.zh === null) {
            console.error(`${page}: Chinese page is missing — nothing to record`);
            failed = true;
            continue;
        }
        if (analysis.structureProblem !== null) {
            console.error(`${page}: ${analysis.structureProblem}`);
            failed = true;
            continue;
        }
        const problems = [
            ...analysis.verbatimDiffs.map(
                ([left, right]) =>
                    `${verbatimLabel(left, right)} must be byte-identical — fix before recording`,
            ),
            ...analysis.labelDiffs.map(labelProblem),
            ...analysis.literalDiffs.map(literalProblem),
        ];
        if (problems.length > 0) {
            for (const problem of problems) {
                console.error(`${page}: ${problem}`);
            }
            failed = true;
            continue;
        }
        const pairs = zip(translatable(analysis.en), translatable(analysis.zh)).map(
            ([left, right]) => ({ kind: left.kind, en: left.hash, zh: right.hash }),
        );
        const serialized = serializeMapping(pairs);
        const file = mappingPath(roots, page);
        if (fs.existsSync(file) && fs.readFileSync(file, "utf8") === serialized) {
            continue;
        }
        const oldEn = new Set(analysis.mapping?.pairs.map((pair) => pair.en) ?? []);
        const oldZh = new Set(analysis.mapping?.pairs.map((pair) => pair.zh) ?? []);
        fs.mkdirSync(path.dirname(file), { recursive: true });
        fs.writeFileSync(file, serialized);
        const enSide = pairs.filter((pair) => !oldEn.has(pair.en)).length;
        const zhSide = pairs.filter((pair) => !oldZh.has(pair.zh)).length;
        const detail =
            analysis.mapping === null ? "new page" : `${enSide} en-side, ${zhSide} zh-side changes`;
        console.log(`${page}: recorded ${pairs.length} pairs (${detail})`);
    }
    for (const stray of findStrays(roots, pages).mappings) {
        fs.rmSync(path.join(roots.meta, stray));
        console.log(`removed stray mapping ${stray}`);
    }
    return failed ? 1 : 0;
}

function parseSegmentsJson(raw: string, expected: number[]): Map<number, string> {
    const cleaned = raw
        .trim()
        .replace(/^```(?:json)?\n?/, "")
        .replace(/\n?```$/, "");
    const data = JSON.parse(cleaned) as { segments?: unknown };
    const out = new Map<number, string>();
    if (Array.isArray(data.segments)) {
        for (const item of data.segments as unknown[]) {
            if (typeof item !== "object" || item === null) {
                continue;
            }
            const entry = item as { i?: unknown; text?: unknown };
            if (typeof entry.i === "number" && typeof entry.text === "string") {
                out.set(entry.i, entry.text.replace(/\n+$/, ""));
            }
        }
    }
    for (const i of expected) {
        if (!out.has(i)) {
            throw new Error(`missing segment ${i} in response`);
        }
    }
    return out;
}

/// Code blocks nested in a translatable segment (a snap example under a
/// capability's text) never round-trip through the model: each
/// is masked to a placeholder and restored byte-for-byte afterwards.
interface MaskedText {
    masked: string;
    blocks: string[];
}

function maskCode(text: string, segment: Segment): MaskedText {
    if (/⟦B\d+⟧/.test(text)) {
        throw new Error("segment already contains placeholder-like text ⟦B…⟧");
    }
    const blocks: string[] = [];
    let masked = "";
    let cursor = 0;
    for (const range of segment.verbatim) {
        const start = range.start - segment.start;
        const end = range.end - segment.start;
        masked += text.slice(cursor, start);
        blocks.push(text.slice(start, end));
        masked += `⟦B${blocks.length}⟧`;
        cursor = end;
    }
    masked += text.slice(cursor);
    return { masked, blocks };
}

function restoreCode(masked: string, blocks: string[]): { text: string } | { problem: string } {
    const seen = new Map<string, number>();
    for (const found of masked.match(/⟦B\d+⟧/g) ?? []) {
        seen.set(found, (seen.get(found) ?? 0) + 1);
    }
    for (let index = 0; index < blocks.length; index += 1) {
        const placeholder = `⟦B${index + 1}⟧`;
        const count = seen.get(placeholder) ?? 0;
        if (count !== 1) {
            return {
                problem: `placeholder ${placeholder} ${count === 0 ? "missing" : "duplicated"}`,
            };
        }
        seen.delete(placeholder);
    }
    if (seen.size > 0) {
        return { problem: `unknown placeholder ${[...seen.keys()].join(" ")}` };
    }
    const text = masked.replace(/⟦B(\d+)⟧/g, (_, n: string) => at(blocks, Number(n) - 1));
    return { text };
}

/// A translated segment re-parsed on its own. A lone row does not parse
/// as a table row: put it under the header and delimiter line the page
/// gives it (dropped from the result again), so a row that would stop
/// the page being a table fails here instead of at the page level.
function parseStandalone(en: Segment, text: string): { probe: string; segments: Segment[] } {
    const align = /^tableRow:\d+:([lrc-]*)$/.exec(en.shape)?.[1];
    if (align === undefined) {
        return { probe: text, segments: splitSegments(text, "reply") };
    }
    const delimiter = (column: string) =>
        column === "l"
            ? " :--- |"
            : column === "r"
              ? " ---: |"
              : column === "c"
                ? " :---: |"
                : " --- |";
    const header = `|${" |".repeat(align.length)}\n|${Array.from(align, delimiter).join("")}\n`;
    const probe = header + text;
    return { probe, segments: splitSegments(probe, "reply").slice(1) };
}

function labelOf(en: Segment, text: string): string | null {
    return parseStandalone(en, text).segments.at(0)?.label ?? null;
}

/// Re-parse the translated segment standalone and reject anything that
/// broke the shape the isomorphism contract depends on or touched a
/// literal the prose must carry over.
function validateSegment(
    en: Segment,
    enText: string,
    zhText: string,
    blocks: string[],
): string | null {
    if (zhText.trim() === "") {
        return "empty";
    }
    if (/\n\s*\n/.test(zhText) && !/\n\s*\n/.test(enText)) {
        return "introduced blank line";
    }
    const { probe, segments } = parseStandalone(en, zhText);
    const reply = segments.at(0);
    if (segments.length !== 1 || reply?.shape !== en.shape) {
        return `not a single ${en.shape}`;
    }
    const code = reply.verbatim.map((range) => probe.slice(range.start, range.end));
    if (code.length !== blocks.length || code.some((text, i) => text !== blocks.at(i))) {
        return "nested verbatim block altered";
    }
    const changes = literalChanges(en.literals, reply.literals);
    if (changes !== null) {
        return `inline literals changed: ${changes}`;
    }
    return null;
}

/// Packs segments into chunks of at most `budget` characters in document
/// order. A paired row and heading travel as one unit at the row's
/// position, so a request always sees both names together.
function chunkSegments(
    indices: number[],
    pairs: [number, number][],
    size: (i: number) => number,
    budget: number,
): number[][] {
    const partner = new Map(pairs);
    const pulled = new Set(pairs.map(([, heading]) => heading));
    const chunks: number[][] = [];
    let current: number[] = [];
    let used = 0;
    for (const i of indices) {
        if (pulled.has(i)) {
            continue;
        }
        const heading = partner.get(i);
        const unit = heading === undefined ? [i] : [i, heading];
        const length = unit.reduce((sum, j) => sum + size(j), 0);
        if (current.length > 0 && used + length > budget) {
            chunks.push(current);
            current = [];
            used = 0;
        }
        current.push(...unit);
        used += length;
    }
    if (current.length > 0) {
        chunks.push(current);
    }
    return chunks;
}

function mustGet<K, V>(map: Map<K, V>, key: K): V {
    const value = map.get(key);
    if (value === undefined) {
        throw new Error(`missing ${String(key)}`);
    }
    return value;
}

/// Indexing that cannot be out of range by construction.
function at<T>(items: T[], i: number): T {
    const item = items[i];
    if (item === undefined) {
        throw new Error(`index ${i} out of range`);
    }
    return item;
}

const REVIEW_PROMPT = `你在审校 clice（一个 C++ 语言服务器）文档的中文译文。输入是一批分段，每段给出编号 i、
markdown 形状 shape、英文原文 en 和当前中文 zh。请逐段判断中文是否准确、术语是否合规、是否自然，
输出每一段的最终中文；已经合格的段原样返回。只输出一个 JSON 对象：
{"segments":[{"i":编号,"text":"最终中文"}, ...]}，每个输入编号都必须出现，不要输出其它内容。

硬性约束（违反会被拒绝）：
- 形如 ⟦B1⟧ 的占位符代表代码块，必须原样保留、各出现恰好一次、不得增删。
- 保持 markdown 形状：标题的 # 个数、列表的标记（- 或 1.）与任务框（- [ ] / - [x]）、表格行的
  竖线数量与列数、引用的 >。段内不要引入空行。
- 行内代码（反引号内）、链接目标、URL、issue 引用（clangd#1455）、文件路径、命令行、编译器
  诊断原文一律原样保留。
- YAML 段（--- 围栏包住的）只改键名为 ${[...YAML_PROSE_KEYS].join("、")} 的字符串值；其余值
  （layout、theme、icon、link、src 等）连同键名、结构、围栏一律不动。
- 不增删信息：中文说英文说的事，不多不少。

术语规则：
- 翻译：页面/章节/能力标题、表头与表格文字、列表项、描述。功能名用固定译名：代码补全、悬停、
  签名帮助、代码导航、文档链接、语义 Token、内联提示、折叠范围、文档符号、格式化、诊断、
  代码操作；Lint 保留。状态词：支持 / 部分支持 / 不支持。
- 有通行中文译名的 C++ 概念翻译（结构化绑定、范围 for 循环、模板特化、显式实例化、折叠表达式、
  参数包、注入类名、概念）；一页中首次出现且英文更利于检索时，用全角括号附英文，
  如 结构化绑定（structured bindings）、最令人烦恼的解析（most vexing parse）。
- 保留英文：产品与工具名（VS Code、Neovim、Zed、CMake、Bazel、clang、clang-format、clangd、
  GCC、MSVC、LLVM）；缩写（LSP、AST、PCH、PCM、CDB、TU、ADL、CTAD、DAG、ABI、URI、C++23）；
  代码字体里的一切；中文 C++ 开发者习惯不译的词（Lambda、Token、Preamble、this、
  作为语言特性名的 Concept）。拿不准时保留英文并加简短中文说明，不要自造译法。
- 同一批里同一术语只用一种译法；同一能力的表格行与标题总在同一批里，两处中文必须完全一致。

位置规则（与 .claude/skills/translate-docs 一致）：
- 标题：散文标题翻译；本身是标识符的标题原样保留（[project]、[[rules]]、textDocument/hover、clice lint）；混合标题只翻散文部分，行内代码原样。
- 能力卡片的名称、一句话摘要、描述都翻译，以代码开头的名称也翻（\`auto\` deduction → \`auto\` 推导）；摘要句末与英文一样不加句号。
- 代码块里的注释也是代码的一部分，绝不翻译。
- 任务列表项翻正文，保留 [ ] / [x]。
- 状态词只用固定词表：支持 / 部分支持 / 不支持 / 已实现 / 存根 / 计划中。

文风：中文句子用全角标点；中英文之间留一个空格；不要机器翻译腔（英文语序、"这个"当冠词、
被动堆叠）；说清楚意思，不必逐词对应。`;

interface ReviewItem {
    i: number;
    shape: string;
    en: string;
    zh: string;
}

type Backend = (payload: string, expected: number[]) => Promise<Map<number, string>>;

/// Runs one codex invocation; the transcript on stdout is dropped, stderr
/// travels with a failure. No stdin: codex would otherwise wait on it for
/// extra input and never start.
function runCodex(args: string[], cwd: string): Promise<void> {
    return new Promise((resolve, reject) => {
        const child = spawn("codex", args, { cwd, stdio: ["ignore", "ignore", "pipe"] });
        let stderr = "";
        child.stderr.on("data", (chunk: Buffer) => {
            stderr += chunk.toString();
        });
        child.on("error", reject);
        child.on("close", (code) => {
            if (code === 0) {
                resolve();
            } else {
                reject(new Error(`codex exited with ${code}: ${stderr.slice(-400)}`));
            }
        });
    });
}

/// The segments are contributor-written text, so the model gets no tool
/// at all: the codex sandbox only stops writes, a shell tool could still
/// read any host file or the environment and hand it to the model. With
/// the shell, exec, subagent, app, image and web-search surfaces off and
/// no MCP servers, the reply the CLI writes through `-o` is the only
/// channel back; the read-only sandbox and the empty scratch directory
/// stay as a second wall.
const CODEX_NO_TOOLS = [
    "--disable",
    "shell_tool",
    "--disable",
    "unified_exec",
    "--disable",
    "multi_agent",
    "--disable",
    "apps",
    "-c",
    "tools.view_image=false",
    "-c",
    'web_search="disabled"',
    "-c",
    "mcp_servers={}",
    "--sandbox",
    "read-only",
];

/// One codex call per chunk.
function codexBackend(effort: string, fast: boolean): Backend {
    return async (payload, expected) => {
        const scratch = fs.mkdtempSync(path.join(os.tmpdir(), "clice-docs-review-"));
        const reply = path.join(scratch, "reply.md");
        try {
            let lastError: unknown = null;
            for (let attempt = 0; attempt < 2; attempt += 1) {
                await runCodex(
                    [
                        "exec",
                        "-m",
                        "gpt-6-astra",
                        "-c",
                        `model_reasoning_effort=${effort}`,
                        ...(fast ? ["-c", "service_tier=fast"] : []),
                        ...CODEX_NO_TOOLS,
                        "-o",
                        reply,
                        `${REVIEW_PROMPT}\n\n输入：\n${payload}`,
                    ],
                    scratch,
                );
                try {
                    return parseSegmentsJson(fs.readFileSync(reply, "utf8"), expected);
                } catch (error) {
                    lastError = error;
                    console.error(`  codex reply unusable, retrying: ${String(error)}`);
                }
            }
            throw lastError instanceof Error ? lastError : new Error(String(lastError));
        } finally {
            fs.rmSync(scratch, { recursive: true, force: true });
        }
    };
}

/// The page's translatable segments paired with their current Chinese,
/// code masked on both sides; chunks are what a backend call reviews.
function reviewChunks(
    roots: Roots,
    page: string,
): {
    items: Map<number, ReviewItem>;
    chunks: number[][];
    enSegments: Segment[];
    zhSource: string;
    zhSegments: Segment[];
    enTexts: string[];
    masks: Map<number, string[]>;
} | null {
    const enSource = fs.readFileSync(path.join(roots.en, page), "utf8");
    const zhFile = path.join(roots.zh, page);
    if (!fs.existsSync(zhFile)) {
        console.error(`${page}: no zh counterpart to review`);
        return null;
    }
    const zhSource = fs.readFileSync(zhFile, "utf8");
    const comparison = compareTrees(
        analyzeSource(enSource, page),
        analyzeSource(zhSource, `zh/${page}`),
    );
    if (comparison.structureProblem !== null) {
        console.error(`${page}: not isomorphic, skipped — ${comparison.structureProblem}`);
        return null;
    }
    const enSegments = splitSegments(enSource, page);
    const zhSegments = splitSegments(zhSource, `zh/${page}`);
    const enTexts = enSegments.map((segment) => enSource.slice(segment.start, segment.end));
    const items = new Map<number, ReviewItem>();
    const masks = new Map<number, string[]>();
    enSegments.forEach((segment, i) => {
        if (!segment.translatable) {
            return;
        }
        const zhSegment = at(zhSegments, i);
        const en = maskCode(at(enTexts, i), segment);
        const zh = maskCode(zhSource.slice(zhSegment.start, zhSegment.end), zhSegment);
        masks.set(i, en.blocks);
        items.set(i, { i, shape: segment.shape, en: en.masked, zh: zh.masked });
    });
    const chunks = chunkSegments(
        [...items.keys()],
        pairedLabels(enSegments),
        (i) => mustGet(items, i).en.length + mustGet(items, i).zh.length,
        6000,
    );
    return { items, chunks, enSegments, zhSource, zhSegments, enTexts, masks };
}

async function reviewPages(roots: Roots, pages: string[], rest: string[]): Promise<number> {
    const flag = (name: string, fallback: string): string =>
        rest.find((argument) => argument.startsWith(`--${name}=`))?.slice(name.length + 3) ??
        fallback;
    const backend = codexBackend(flag("effort", "xhigh"), rest.includes("--fast"));
    const limit = pLimit(Number(flag("jobs", "4")));
    const requested = [...new Set(rest.filter((argument) => !argument.startsWith("--")))];
    const translatablePages = pages.filter((page) => !isUntranslated(page));
    const unknown = requested.filter((page) => !translatablePages.includes(page));
    if (unknown.length > 0) {
        console.error(`not a translatable page under docs/en: ${unknown.join(", ")}`);
        return 2;
    }
    const targets = requested.length > 0 ? requested : translatablePages;

    let failed = 0;
    const work = targets.map(async (page) => {
        const plan = reviewChunks(roots, page);
        if (plan === null) {
            failed += 1;
            return;
        }
        const { items, chunks, enSegments, zhSource, zhSegments, enTexts, masks } = plan;
        const replies = await Promise.all(
            chunks.map((chunk) =>
                limit(async () => {
                    const payload = JSON.stringify({
                        segments: chunk.map((i) => mustGet(items, i)),
                    });
                    return backend(payload, chunk);
                }),
            ),
        );
        const reviewed = new Map<number, string>();
        for (const reply of replies) {
            for (const [i, text] of reply) {
                reviewed.set(i, text);
            }
        }
        const currentText = (i: number) =>
            zhSource.slice(at(zhSegments, i).start, at(zhSegments, i).end);
        const finalTexts = new Map<number, string>();
        let kept = 0;
        for (const item of items.values()) {
            const blocks = mustGet(masks, item.i);
            const restored = restoreCode(mustGet(reviewed, item.i), blocks);
            const problem =
                "problem" in restored
                    ? restored.problem
                    : validateSegment(
                          at(enSegments, item.i),
                          at(enTexts, item.i),
                          restored.text,
                          blocks,
                      );
            if ("problem" in restored || problem !== null) {
                console.error(`  ${page} segment ${item.i + 1} (${item.shape}): ${problem} — kept`);
                kept += 1;
                finalTexts.set(item.i, currentText(item.i));
                continue;
            }
            finalTexts.set(item.i, restored.text);
        }
        for (const [row, heading] of pairedLabels(enSegments)) {
            const rowLabel = labelOf(at(enSegments, row), mustGet(finalTexts, row));
            if (rowLabel === labelOf(at(enSegments, heading), mustGet(finalTexts, heading))) {
                continue;
            }
            console.error(
                `  ${page} segments ${row + 1} and ${heading + 1}: table row and heading ` +
                    `share one name in en but came back different — kept`,
            );
            for (const i of [row, heading]) {
                if (mustGet(finalTexts, i) !== currentText(i)) {
                    finalTexts.set(i, currentText(i));
                    kept += 1;
                }
            }
        }
        const changed = [...finalTexts].filter(([i, text]) => text !== currentText(i)).length;
        let out = "";
        let cursor = 0;
        zhSegments.forEach((segment, i) => {
            out += zhSource.slice(cursor, segment.start);
            out += finalTexts.has(i)
                ? mustGet(finalTexts, i)
                : zhSource.slice(segment.start, segment.end);
            cursor = segment.end;
        });
        out += zhSource.slice(cursor);
        const enSource = fs.readFileSync(path.join(roots.en, page), "utf8");
        const comparison = compareTrees(
            analyzeSource(enSource, page),
            analyzeSource(out, `zh/${page}`),
        );
        if (comparison.structureProblem !== null) {
            throw new Error(
                `${page}: reviewed page is not isomorphic — ${comparison.structureProblem}`,
            );
        }
        for (const [left, right] of comparison.verbatimDiffs) {
            throw new Error(`${page}: ${verbatimLabel(left, right)} corrupted`);
        }
        for (const diff of comparison.labelDiffs) {
            throw new Error(`${page}: ${labelProblem(diff)}`);
        }
        for (const diff of comparison.literalDiffs) {
            throw new Error(`${page}: ${literalProblem(diff)}`);
        }
        fs.writeFileSync(path.join(roots.zh, page), out);
        console.log(
            `done ${page}: ${items.size} segments, ${changed} changed, ${kept} kept on problems`,
        );
    });
    for (const [i, outcome] of (await Promise.allSettled(work)).entries()) {
        if (outcome.status === "rejected") {
            const reason =
                outcome.reason instanceof Error ? outcome.reason.message : String(outcome.reason);
            console.error(`FAILED ${targets.at(i) ?? "?"}: ${reason}`);
            failed += 1;
        }
    }
    console.log(`finished: ${targets.length - failed} pages ok, ${failed} failed`);
    return failed > 0 ? 1 : 0;
}

async function main(): Promise<number> {
    const [mode, ...rest] = process.argv.slice(2);
    const flag = (name: string, fallback: string): string => {
        const match = rest.find((argument) => argument.startsWith(`--${name}=`));
        return match === undefined ? fallback : match.slice(name.length + 3);
    };
    const roots: Roots = {
        en: flag("en", path.join(REPO_ROOT, "docs/en")),
        zh: flag("zh", path.join(REPO_ROOT, "docs/zh")),
        meta: flag("meta", path.join(REPO_ROOT, "docs/meta/translations")),
    };
    const pages = listFiles(roots.en, ".md");
    switch (mode) {
        case "check":
            return check(roots, pages);
        case "report":
            return report(roots, pages);
        case "record":
            return record(roots, pages);
        case "review":
            return reviewPages(roots, pages, rest);
        default:
            console.error("usage: node tools/docs/translate.ts check | report | record | review");
            return 2;
    }
}

process.exit(await main());
