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
/// `--en=DIR --zh=DIR --meta=DIR` override the tree roots (for testing).

import * as fs from "node:fs";
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
                files.push(path.relative(root, full));
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
function serializeMapping(pairs: Pair[]): string {
    if (pairs.length === 0) {
        return `{\n  "version": 1,\n  "pairs": []\n}\n`;
    }
    const lines = pairs.map(
        (pair) =>
            `    { "kind": ${JSON.stringify(pair.kind)}, ` +
            `"en": ${JSON.stringify(pair.en)}, "zh": ${JSON.stringify(pair.zh)} }`,
    );
    return `{\n  "version": 1,\n  "pairs": [\n${lines.join(",\n")}\n  ]\n}\n`;
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
        ? `code block inside ${segmentLabel(left, right)}`
        : `verbatim ${segmentLabel(left, right)}`;
}

function verbatimReason(left: SegmentInfo): string {
    return left.translatable
        ? "nested code block must be byte-identical"
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
            const recorded = new Set(analysis.mapping.pairs.map((pair) => `${pair.en}:${pair.zh}`));
            console.log(
                `${page}: layout changed since last record ` +
                    `(${analysis.mapping.pairs.length} pairs recorded, ` +
                    `${pairsNow.length} segments now); segments not covered:`,
            );
            for (const [left, right] of pairsNow) {
                if (!recorded.has(`${left.hash}:${right.hash}`)) {
                    reportDrift(left, right, "no recorded pair");
                    pageDrifts += 1;
                }
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

/// Code blocks nested in a translatable segment (snap example blocks live
/// inside their checklist items) never round-trip through the model: each
/// is masked to a placeholder and restored byte-for-byte afterwards.
interface MaskedText {
    masked: string;
    blocks: string[];
}

function maskCode(text: string, segment: Segment): MaskedText {
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
    let text = masked;
    for (const [index, block] of blocks.entries()) {
        const placeholder = `⟦B${index + 1}⟧`;
        const first = text.indexOf(placeholder);
        if (first === -1) {
            return { problem: `placeholder ${placeholder} missing` };
        }
        if (text.includes(placeholder, first + placeholder.length)) {
            return { problem: `placeholder ${placeholder} duplicated` };
        }
        text = text.replace(placeholder, () => block);
    }
    if (/⟦B\d+⟧/.test(text)) {
        return { problem: "unknown placeholder left in output" };
    }
    return { text };
}

/// Re-parse the translated segment standalone and reject anything that
/// broke the shape the isomorphism contract depends on.
function validateSegment(en: Segment, enText: string, zhText: string): string | null {
    if (zhText.trim() === "") {
        return "empty";
    }
    if (/\n\s*\n/.test(zhText) && !/\n\s*\n/.test(enText)) {
        return "introduced blank line";
    }
    if (en.kind === "tableRow") {
        // A lone row does not parse as a table, so check its skeleton.
        if (zhText.includes("\n")) {
            return "table row must stay one line";
        }
        const pipes = (text: string) => (text.match(/(?<!\\)\|/g) ?? []).length;
        if (pipes(zhText) !== pipes(enText)) {
            return "pipe count changed";
        }
        return null;
    }
    const parsed = splitSegments(zhText, "reply");
    if (parsed.length !== 1 || parsed.at(0)?.shape !== en.shape) {
        return `not a single ${en.shape}`;
    }
    if (en.kind === "yaml") {
        const keys = (text: string) =>
            [...text.matchAll(/^\s*([\w-]+):/gm)].map((match) => match[1]).join(",");
        if (keys(zhText) !== keys(enText)) {
            return "yaml keys changed";
        }
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
                : validateSegment(segment, enText, restored.text);
        for (let round = 0; problem !== null && round < 2; round += 1) {
            console.error(`  ${page} segment ${i + 1} (${segment.shape}): ${problem} — retrying`);
            try {
                const reply = await retrySegment(model, context, at(maskedTexts, i), i, problem);
                restored = restoreCode(reply, blocks);
                problem =
                    "problem" in restored
                        ? restored.problem
                        : validateSegment(segment, enText, restored.text);
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
    console.log(`finished: ${targets.length - failed} ok, ${failed} failed`);
    return failed > 0 || incomplete.length > 0 ? 1 : 0;
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
        default:
            console.error(
                "usage: node tools/docs/translate.ts check | report | record | translate",
            );
            return 2;
    }
}

process.exit(await main());
