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
/// nested inside a translatable segment. The only stored link between
/// the two sides is docs/meta/translations/<page>.json — one hash pair
/// per translatable segment, in document order:
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
///     node tools/docs/translate.ts translate [page...]  # machine-draft zh pages
///     node tools/docs/translate.ts review [page...]     # model review of existing zh pages
///
/// `translate` calls the DeepSeek API (key from DEEPSEEK_API_KEY, never
/// stored) to draft segment-isomorphic zh pages: fenced code inside a
/// segment is masked out and restored byte-for-byte, so code never
/// round-trips through the model. No args = only pages missing a zh
/// counterpart; explicit pages are overwritten, feeding the current zh
/// text back as terminology reference. `--model=NAME` overrides the
/// default deepseek-v4-pro. A segment the model cannot render validly is
/// left in English and fails the run, so the page shows up again on the
/// next attempt. Drafts still go through review + record.
///
/// `review` re-reads every translatable segment of an existing zh page
/// next to its en counterpart and asks a model for the corrected Chinese —
/// meaning, the wording conventions of the docs skill, naturalness —
/// segment by segment, so no code block ever enters the model's context.
/// The default backend runs the codex CLI (GPT-5.6-sol) from an empty
/// scratch directory, one call per chunk of segments, `--jobs=N` calls in
/// parallel and `--effort=LEVEL` reasoning; `--backend=deepseek` uses the
/// API instead. A reply that breaks a segment's shape keeps the current
/// Chinese. The pages are rewritten in place; review the diff, then
/// `record`.
///
/// `--en=DIR --zh=DIR --meta=DIR` override the tree roots (for testing).

import { spawn } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import pLimit from "p-limit";
import { REPO_ROOT } from "../compile_commands.ts";
import { analyzeSource, splitSegments, type Segment, type SegmentInfo } from "./segment.ts";

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

interface TreeComparison {
    /// Human-readable description of a block-layout divergence, or null
    /// when the two sides are isomorphic.
    structureProblem: string | null;
    /// Segment pairs whose verbatim bytes differ (structure was isomorphic).
    verbatimDiffs: [SegmentInfo, SegmentInfo][];
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
            };
        }
    }
    return {
        structureProblem: null,
        verbatimDiffs: zip(en, zh).filter(([left, right]) => !sameVerbatim(left, right)),
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
        return { page, en, zh: null, mapping, structureProblem: null, verbatimDiffs: [] };
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
        const drifted = (left: SegmentInfo, right: SegmentInfo, reason: string) => {
            if (pageDrifts === 0) {
                console.log(`${page}:`);
            }
            reportDrift(left, right, reason);
            pageDrifts += 1;
        };
        for (const [left, right] of analysis.verbatimDiffs) {
            drifted(left, right, verbatimReason(left));
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
        if (analysis.verbatimDiffs.length > 0) {
            for (const [left, right] of analysis.verbatimDiffs) {
                console.error(
                    `${page}: ${verbatimLabel(left, right)} ` +
                        `must be byte-identical — fix before recording`,
                );
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

const SYSTEM_PROMPT = `你是 clice 项目的文档翻译。clice 是一个基于 LLVM/Clang 的 C++ language server。你的任务是把英文技术文档段落翻译成简体中文。

写作要求：
- 写成中国系统程序员自然写出的技术文档：直接、准确、克制，参考译文的语气优先沿用。
- 禁止翻译腔：不要堆叠"进行""对于""通过……的方式"；长定语从句拆成短句；不要滥用"我们"。
- 禁用词：深入、强大、无缝、赋能、极大地、显著地、值得注意的是、总而言之、综上所述。
- 术语保留英文原样：preamble、PCH、PCM、AST、TU、worker、master、LSP、hover、completion、token、fixture、snapshot、shard、Clang、LLVM、clangd、以及一切行内代码、类名、函数名、文件路径、命令、URL。
- 惯用译法：translation unit→翻译单元、compilation database→编译数据库、header→头文件、index→索引、crash→崩溃、build→构建、language server 不翻。旧译文里已有的术语选择优先沿用。

格式硬规则（违反即作废）：
- 逐段翻译：输入 segments 数组，输出同样长度的数组，i 一一对应，绝不合并、拆分、增删段。
- 每段保持 markdown 骨架：标题保持相同数量的 #；列表项保持"- "或数字"1. "前缀和嵌套缩进结构；表格行保持竖线数量与单元格结构；引用块每行保持"> "前缀；行内代码、粗体、链接语法原样，链接 URL 绝不改。
- 段内不得引入空行（空行会把一段拆成两段）。
- 输入里的 ⟦B数字⟧ 是被抽走的代码块占位符：在译文的对应位置原样保留，一个不能少、不能多、不能改。
- YAML 段（--- 围栏包住的）：只翻译面向读者的文案值（text、title、tagline、details 等），键名、结构、路径、链接一律不动，围栏保留。
- 输出严格 JSON：{"segments":[{"i":<int>,"text":"<译文>"}, ...]}，不要任何解释或代码围栏。`;

interface ChatMessage {
    role: "system" | "user";
    content: string;
}

interface ApiReply {
    content: string;
    truncated: boolean;
}

interface ChatCompletion {
    choices: { message: { content: string }; finish_reason: string }[];
}

function apiKey(): string {
    const key = process.env["DEEPSEEK_API_KEY"];
    if (key === undefined || key === "") {
        console.error("DEEPSEEK_API_KEY not set");
        process.exit(2);
    }
    return key;
}

async function callApi(
    model: string,
    messages: ChatMessage[],
    maxTokens: number,
): Promise<ApiReply> {
    for (let attempt = 0; attempt < 4; attempt += 1) {
        try {
            const response = await fetch("https://api.deepseek.com/chat/completions", {
                method: "POST",
                headers: {
                    "Content-Type": "application/json",
                    Authorization: `Bearer ${apiKey()}`,
                },
                body: JSON.stringify({
                    model,
                    messages,
                    temperature: 1.3,
                    max_tokens: maxTokens,
                    response_format: { type: "json_object" },
                }),
            });
            if (!response.ok) {
                throw new Error(
                    `HTTP ${response.status}: ${(await response.text()).slice(0, 200)}`,
                );
            }
            const data = (await response.json()) as ChatCompletion;
            const choice = data.choices.at(0);
            if (choice === undefined) {
                throw new Error("empty choices in response");
            }
            return {
                content: choice.message.content,
                truncated: choice.finish_reason === "length",
            };
        } catch (error) {
            if (attempt === 3) {
                throw error;
            }
            const wait = 2000 * (attempt + 1) ** 2;
            console.error(`  api retry in ${wait}ms: ${String(error)}`);
            await new Promise((resolve) => setTimeout(resolve, wait));
        }
    }
    throw new Error("unreachable");
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

/// Re-parse the translated segment standalone and reject anything that
/// broke the shape the isomorphism contract depends on.
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
    // A lone row does not parse as a table: give it the delimiter line
    // the page will, so a row that would stop the page being a table
    // fails here instead of at the page level.
    const align = /^tableRow:\d+:([lrc-]*)$/.exec(en.shape)?.[1];
    const delimiter = (column: string) =>
        column === "l"
            ? " :--- |"
            : column === "r"
              ? " ---: |"
              : column === "c"
                ? " :---: |"
                : " --- |";
    const probe =
        align === undefined ? zhText : `${zhText}\n|${Array.from(align, delimiter).join("")}`;
    const parsed = splitSegments(probe, "reply");
    const reply = parsed.at(0);
    if (parsed.length !== 1 || reply?.shape !== en.shape) {
        return `not a single ${en.shape}`;
    }
    const code = reply.verbatim.map((range) => probe.slice(range.start, range.end));
    if (code.length !== blocks.length || code.some((text, i) => text !== blocks.at(i))) {
        return "nested verbatim block altered";
    }
    return null;
}

function chunkIndices(texts: string[], indices: number[], budget: number): number[][] {
    const chunks: number[][] = [];
    let current: number[] = [];
    let size = 0;
    for (const i of indices) {
        const length = texts.at(i)?.length ?? 0;
        if (current.length > 0 && size + length > budget) {
            chunks.push(current);
            current = [];
            size = 0;
        }
        current.push(i);
        size += length;
    }
    if (current.length > 0) {
        chunks.push(current);
    }
    return chunks;
}

function contextBlock(enSource: string, reference: string | null): string {
    const clip = (text: string, limit: number) =>
        text.length > limit ? text.slice(0, limit) + "\n…（截断）" : text;
    return (
        `【英文原页，只读上下文】\n${clip(enSource, 24000)}\n\n` +
        `【旧中文版本，术语与语气参考，内容可能过时、结构不要照抄】\n` +
        (reference === null ? "（无）" : clip(reference, 24000))
    );
}

function mustGet<K, V>(map: Map<K, V>, key: K): V {
    const value = map.get(key);
    if (value === undefined) {
        throw new Error(`missing entry ${String(key)}`);
    }
    return value;
}

function at<T>(items: T[], i: number): T {
    const value = items.at(i);
    if (value === undefined) {
        throw new Error(`index ${i} out of range`);
    }
    return value;
}

async function translateChunk(
    model: string,
    context: string,
    texts: string[],
    indices: number[],
): Promise<Map<number, string>> {
    const payload = { segments: indices.map((i) => ({ i, text: at(texts, i) })) };
    const reply = await callApi(
        model,
        [
            { role: "system", content: SYSTEM_PROMPT },
            { role: "user", content: `${context}\n\n【待翻译段】\n${JSON.stringify(payload)}` },
        ],
        8000,
    );
    try {
        if (reply.truncated) {
            throw new Error("output truncated");
        }
        return parseSegmentsJson(reply.content, indices);
    } catch (error) {
        // Output overflow truncates the JSON; halve the chunk and retry.
        if (indices.length === 1) {
            throw error;
        }
        console.error(
            `  chunk of ${indices.length} segments failed (${String(error)}) — splitting`,
        );
        const half = Math.ceil(indices.length / 2);
        const left = await translateChunk(model, context, texts, indices.slice(0, half));
        const right = await translateChunk(model, context, texts, indices.slice(half));
        return new Map([...left, ...right]);
    }
}

async function retrySegment(
    model: string,
    context: string,
    enText: string,
    i: number,
    problem: string,
): Promise<string> {
    const payload = { segments: [{ i, text: enText }] };
    const reply = await callApi(
        model,
        [
            { role: "system", content: SYSTEM_PROMPT },
            {
                role: "user",
                content:
                    `${context}\n\n【待翻译段】\n${JSON.stringify(payload)}\n\n` +
                    `上一次译文违反了格式硬规则（${problem}），重新翻译这一段并严格保持骨架。`,
            },
        ],
        4000,
    );
    return mustGet(parseSegmentsJson(reply.content, [i]), i);
}

interface PageResult {
    segments: number;
    fallbacks: number;
}

async function translatePage(roots: Roots, model: string, page: string): Promise<PageResult> {
    const source = fs.readFileSync(path.join(roots.en, page), "utf8");
    const segments = splitSegments(source, page);
    const texts = segments.map((segment) => source.slice(segment.start, segment.end));
    const zhFile = path.join(roots.zh, page);
    const reference = fs.existsSync(zhFile) ? fs.readFileSync(zhFile, "utf8") : null;
    const context = contextBlock(source, reference);

    const todo: number[] = [];
    segments.forEach((segment, i) => {
        if (segment.translatable) {
            todo.push(i);
        }
    });
    const maskedTexts = texts.slice();
    const masks = new Map<number, string[]>();
    for (const i of todo) {
        const { masked, blocks } = maskCode(at(texts, i), at(segments, i));
        maskedTexts[i] = masked;
        masks.set(i, blocks);
    }
    const translations = new Map<number, string>();
    for (const chunk of chunkIndices(maskedTexts, todo, 4500)) {
        for (const [i, text] of await translateChunk(model, context, maskedTexts, chunk)) {
            translations.set(i, text);
        }
    }

    let fallbacks = 0;
    for (const i of todo) {
        const enText = at(texts, i);
        const segment = at(segments, i);
        const blocks = mustGet(masks, i);
        let restored = restoreCode(mustGet(translations, i), blocks);
        let problem =
            "problem" in restored
                ? restored.problem
                : validateSegment(segment, enText, restored.text, blocks);
        for (let round = 0; problem !== null && round < 2; round += 1) {
            console.error(`  ${page} segment ${i + 1} (${segment.shape}): ${problem} — retrying`);
            try {
                const reply = await retrySegment(model, context, at(maskedTexts, i), i, problem);
                restored = restoreCode(reply, blocks);
                problem =
                    "problem" in restored
                        ? restored.problem
                        : validateSegment(segment, enText, restored.text, blocks);
            } catch (error) {
                problem = String(error);
            }
        }
        let zhText = "text" in restored ? restored.text : enText;
        if (problem !== null) {
            console.error(
                `  ${page} segment ${i + 1}: still broken (${problem}) — keeping English`,
            );
            zhText = enText;
            fallbacks += 1;
        }
        translations.set(i, zhText);
    }

    let out = "";
    let cursor = 0;
    segments.forEach((segment, i) => {
        out += source.slice(cursor, segment.start);
        out += segment.translatable ? mustGet(translations, i) : at(texts, i);
        cursor = segment.end;
    });
    out += source.slice(cursor);

    const comparison = compareTrees(analyzeSource(source, page), analyzeSource(out, `zh/${page}`));
    if (comparison.structureProblem !== null) {
        throw new Error(
            `${page}: assembled page is not isomorphic — ${comparison.structureProblem}`,
        );
    }
    for (const [left, right] of comparison.verbatimDiffs) {
        throw new Error(`${page}: ${verbatimLabel(left, right)} corrupted`);
    }

    fs.mkdirSync(path.dirname(zhFile), { recursive: true });
    fs.writeFileSync(zhFile, out);
    return { segments: todo.length, fallbacks };
}

async function machineTranslate(roots: Roots, pages: string[], rest: string[]): Promise<number> {
    const model =
        rest.find((argument) => argument.startsWith("--model="))?.slice(8) ?? "deepseek-v4-pro";
    const requested = [...new Set(rest.filter((argument) => !argument.startsWith("--")))];
    const translatablePages = pages.filter((page) => !isUntranslated(page));
    const unknown = requested.filter((page) => !translatablePages.includes(page));
    if (unknown.length > 0) {
        console.error(`not a translatable page under docs/en: ${unknown.join(", ")}`);
        return 2;
    }
    const targets =
        requested.length > 0
            ? requested
            : translatablePages.filter((page) => !fs.existsSync(path.join(roots.zh, page)));
    if (targets.length === 0) {
        console.log("nothing to translate: every page has a zh counterpart");
        return 0;
    }
    console.log(`translating ${targets.length} pages with ${model}`);
    const limit = pLimit(3);
    const incomplete: string[] = [];
    const results = await Promise.allSettled(
        targets.map((page) =>
            limit(async () => {
                const started = Date.now();
                const info = await translatePage(roots, model, page);
                const seconds = Math.round((Date.now() - started) / 1000);
                console.log(
                    `done ${page}: ${info.segments} segments, ` +
                        `${info.fallbacks} fallbacks, ${seconds}s`,
                );
                if (info.fallbacks > 0) {
                    incomplete.push(page);
                }
            }),
        ),
    );
    let failed = 0;
    for (const [i, result] of results.entries()) {
        if (result.status === "rejected") {
            const reason =
                result.reason instanceof Error ? result.reason.message : String(result.reason);
            console.error(`FAILED ${targets.at(i) ?? "?"}: ${reason}`);
            failed += 1;
        }
    }
    if (incomplete.length > 0) {
        console.error(
            `segments left in English on ${incomplete.length} pages — review, then rerun ` +
                `translate on: ${incomplete.sort().join(" ")}`,
        );
    }
    const ok = targets.length - failed - incomplete.length;
    console.log(`finished: ${ok} ok, ${incomplete.length} incomplete, ${failed} failed`);
    return failed > 0 || incomplete.length > 0 ? 1 : 0;
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
- 同一批里同一术语只用一种译法；同一能力在表格行和标题里必须完全一致。

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

/// One codex call per chunk, from an empty scratch directory so the model
/// has nothing to read but the prompt; the reply arrives through `-o`.
function codexBackend(effort: string): Backend {
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
                        "gpt-5.6-sol",
                        "-c",
                        `model_reasoning_effort=${effort}`,
                        "--dangerously-bypass-approvals-and-sandbox",
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

function deepseekBackend(model: string): Backend {
    return async (payload, expected) => {
        const reply = await callApi(
            model,
            [
                { role: "system", content: REVIEW_PROMPT },
                { role: "user", content: payload },
            ],
            8192,
        );
        if (reply.truncated) {
            throw new Error("reply truncated");
        }
        return parseSegmentsJson(reply.content, expected);
    };
}

interface ReviewResult {
    reviewed: number;
    changed: number;
    kept: number;
}

/// The page's translatable segments paired with their current Chinese,
/// code masked on both sides; chunks are what a backend call reviews.
function reviewChunks(
    roots: Roots,
    page: string,
): {
    items: ReviewItem[];
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
    const items: ReviewItem[] = [];
    const masks = new Map<number, string[]>();
    enSegments.forEach((segment, i) => {
        if (!segment.translatable) {
            return;
        }
        const zhSegment = at(zhSegments, i);
        const en = maskCode(at(enTexts, i), segment);
        const zh = maskCode(zhSource.slice(zhSegment.start, zhSegment.end), zhSegment);
        masks.set(i, en.blocks);
        items.push({ i, shape: segment.shape, en: en.masked, zh: zh.masked });
    });
    const sizes = items.map((item) => item.en.length + item.zh.length);
    const chunks = chunkIndices(
        sizes.map((n) => "x".repeat(n)),
        items.map((_, k) => k),
        6000,
    );
    return { items, chunks, enSegments, zhSource, zhSegments, enTexts, masks };
}

async function reviewPages(roots: Roots, pages: string[], rest: string[]): Promise<number> {
    const flag = (name: string, fallback: string): string =>
        rest.find((argument) => argument.startsWith(`--${name}=`))?.slice(name.length + 3) ??
        fallback;
    const backend =
        flag("backend", "codex") === "deepseek"
            ? deepseekBackend(flag("model", "deepseek-v4-pro"))
            : codexBackend(flag("effort", "xhigh"));
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
                    const payload = JSON.stringify({ segments: chunk.map((k) => at(items, k)) });
                    const expected = chunk.map((k) => at(items, k).i);
                    return backend(payload, expected);
                }),
            ),
        );
        const reviewed = new Map<number, string>();
        for (const reply of replies) {
            for (const [i, text] of reply) {
                reviewed.set(i, text);
            }
        }
        const result: ReviewResult = { reviewed: items.length, changed: 0, kept: 0 };
        const finalTexts = new Map<number, string>();
        for (const item of items) {
            const blocks = mustGet(masks, item.i);
            const current = zhSource.slice(
                at(zhSegments, item.i).start,
                at(zhSegments, item.i).end,
            );
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
                result.kept += 1;
                finalTexts.set(item.i, current);
                continue;
            }
            if (restored.text !== current) {
                result.changed += 1;
            }
            finalTexts.set(item.i, restored.text);
        }
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
        fs.writeFileSync(path.join(roots.zh, page), out);
        console.log(
            `done ${page}: ${result.reviewed} segments, ${result.changed} changed, ${result.kept} kept on problems`,
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
        case "translate":
            return machineTranslate(roots, pages, rest);
        case "review":
            return reviewPages(roots, pages, rest);
        default:
            console.error(
                "usage: node tools/docs/translate.ts check | report | record | translate | review",
            );
            return 2;
    }
}

process.exit(await main());
