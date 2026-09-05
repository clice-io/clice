/// Corpus model of the snap domain — the single source of truth for what a
/// fixture is: enumeration and unit detection, the strict frontmatter
/// schema, the corpus.json manifest, and materialization of one fixture
/// into a throwaway workspace for the server driver.
///
/// A fixture is either a single `.cpp` or a subdirectory entered through
/// its `main.cpp` — one multi-file unit whose sibling sources (module
/// interfaces, headers, extra sources) belong to the fixture. Fixtures live
/// at the corpus root or in a section directory: the directory names the
/// doc page's generated region its items render into, and their numbered
/// file names (`NN_name.cpp`, `NN_unit/main.cpp`) order them there.
/// Everything else in the corpus is support material shared by all
/// fixtures (include roots, `.clang-format`, ...).

import * as fs from "node:fs";
import * as path from "node:path";
import { SNAP_DIR } from "../compile_commands.ts";
import { parseAnnotations, type AnnotatedSource } from "./annotation.ts";

/// Fixture doc-header metadata that controls the snap suite. Parsing is
/// strict: an unknown key is an error, not a silently ignored typo — a
/// misspelled `verify:` would otherwise disable the shared-snapshot
/// assertion for that fixture without anyone noticing.
export interface FixtureMeta {
    status: "supported" | "partial" | "unsupported";
    /// Which verification paths run the fixture: `both` (the default) runs
    /// `clice inspect` and a real server, `inspect`/`server` only the one
    /// path (e.g. include and import completion exist only in the server;
    /// index dumps are inspect-only).
    verify: "both" | "inspect" | "server";
    /// How the two paths of a `verify: both` fixture relate. shared: they
    /// must render byte-identically and are pinned by one snapshot file.
    /// separate: the difference is a known property of the feature; each
    /// path pins its own file. skip: the paths disagree in a way that is
    /// simply wrong — the fixture runs nowhere and keeps no snapshot until
    /// fixed.
    snap: "shared" | "separate" | "skip";
    /// Feature-options overlay as a JSON object (the body of the feature's
    /// config section). The snapshot pins BOTH halves: the default options
    /// and the overlaid ones, as `default:` / `configured:` blocks.
    config?: string;
    /// `- diagnostics: expected` — the fixture deliberately does not
    /// compile cleanly. Diagnostics without it fail the fixture, and so
    /// does a clean compile with it.
    diagnostics: boolean;
    /// Enables background indexing on the server path (off by default —
    /// most fixtures recompute from open documents and skipping the index
    /// keeps the suite fast).
    indexing: boolean;
    /// Extra compile flags for this fixture, appended to the corpus flags.
    flags: string[];
}

/// The keys a fixture's doc header may carry — the one vocabulary the snap
/// suite and the docs generator both validate against. `issues` is
/// rendered into the docs only; `snap` and `config` are read by the
/// snapshot suites only.
export const FIXTURE_META_KEYS: readonly string[] = [
    "status",
    "issues",
    "verify",
    "snap",
    "config",
    "diagnostics",
    "indexing",
    "flags",
];

/// A fixture's leading `///` block, split into its parts. Documented
/// fixtures use one `#` heading, a fixed-order metadata list, and prose
/// whose first paragraph is the summary. Supplementary fixtures may use a
/// metadata-only block while the corpus is being read; the feature-doc
/// validator rejects that legacy spelling under R7.
export interface FixtureHeader {
    /// Every line in the file. Indices in this structure are whole-file
    /// indices because snapshot positions use whole-file line numbers.
    lines: string[];
    /// Index of the first `///` header line, or 0 when there is no header.
    headerStart: number;
    /// The markdown heading lines opening the block, comment prefix
    /// stripped.
    headings: string[];
    /// One-based source lines corresponding to `headings`.
    headingLines: number[];
    /// The capability name from the opening `#` heading.
    name: string;
    /// Whether the title is followed by the required blank `///` line.
    titleSeparated: boolean;
    /// The `- key: value` list after the headings, in order; keys and
    /// duplicates unchecked.
    meta: { key: string; value: string; line: number }[];
    /// Lines inside the list that are not entries: a misspelled entry must
    /// error, not silently end the list on defaults.
    malformed: { text: string; line: number }[];
    /// Whether metadata is followed by the required blank `///` line.
    proseSeparated: boolean;
    /// One-based line where the prose separator belongs.
    proseSeparatorLine: number;
    /// The first prose paragraph, rendered as the capability summary.
    summary: string;
    /// Located lines in the summary paragraph.
    summaryLines: { text: string; line: number }[];
    /// The stripped `///` lines after the summary paragraph.
    description: string[];
    /// Located lines corresponding to `description`.
    descriptionLines: { text: string; line: number }[];
    /// The text of a directly-following `// snap:` block, prefix stripped.
    notes: string[];
    /// Index into `lines` of the first line after the block.
    bodyStart: number;
}

/// Split text into lines the way Python's str.splitlines() does: on any of
/// \r\n, \r or \n, without a trailing empty element for a final line break.
function splitLines(text: string): string[] {
    if (text === "") {
        return [];
    }
    const lines = text.split(/\r\n|\r|\n/);
    if (lines[lines.length - 1] === "" && /[\r\n]$/.test(text)) {
        lines.pop();
    }
    return lines;
}

/// The text of a `///` comment line, minus the prefix and one space.
function stripComment(line: string): string {
    let text = line.trimStart().slice(3);
    if (text.startsWith(" ")) {
        text = text.slice(1);
    }
    return text;
}

const HEADING_RE = /^(#{1,6})(?:\s|$)/;
const META_RE = /^-\s+(\w+):\s*(.*)$/;
/// Looser than META_RE on purpose: a misspelled entry (`- Snap:`, `- snap :`)
/// must open the header and error rather than leave the fixture silently on
/// defaults, while a bullet without a colon is prose.
const ENTRY_RE = /^-\s+[^:]+:/;

/// The level of a markdown heading line (`## Title` is 2); 0 for any
/// other text.
export function headingLevel(text: string): number {
    return HEADING_RE.exec(text)?.[1]?.length ?? 0;
}

export function scanFixtureHeader(content: string): FixtureHeader {
    const all = splitLines(content);
    const header: FixtureHeader = {
        lines: all,
        headerStart: 0,
        headings: [],
        headingLines: [],
        name: "",
        titleSeparated: false,
        meta: [],
        malformed: [],
        proseSeparated: false,
        proseSeparatorLine: 1,
        summary: "",
        summaryLines: [],
        description: [],
        descriptionLines: [],
        notes: [],
        bodyStart: 0,
    };

    const firstPlain = (all[0] ?? "").trimStart();
    if (firstPlain.startsWith("//") && !firstPlain.startsWith("///")) {
        const firstText = firstPlain.slice(2).trimStart();
        if (ENTRY_RE.test(firstText)) {
            let edge = 0;
            while (edge < all.length) {
                const raw = (all[edge] ?? "").trimStart();
                if (!raw.startsWith("//") || raw.startsWith("///")) {
                    break;
                }
                const text = raw.slice(2).trimStart();
                if (text === "") {
                    break;
                }
                const match = META_RE.exec(text);
                if (match) {
                    header.meta.push({
                        key: match[1] ?? "",
                        value: (match[2] ?? "").trim(),
                        line: edge + 1,
                    });
                } else {
                    header.malformed.push({ text, line: edge + 1 });
                }
                edge += 1;
            }
            header.bodyStart = edge;
            return header;
        }
    }

    let prologue = 0;
    while (prologue < all.length) {
        const line = (all[prologue] ?? "").trim();
        if (line !== "" && !(line.startsWith("//") && !line.startsWith("///"))) {
            break;
        }
        prologue += 1;
    }
    let i = prologue;
    const comment = (): string | null => {
        const raw = all[i];
        return raw?.trimStart().startsWith("///") ? stripComment(raw) : null;
    };
    // A header opens with a heading or an entry (blank `///` lines before
    // it are skipped). Any other leading `///` block is an ordinary doc
    // comment on the code — the fixture has no header.
    let opening = 0;
    while (
        (all[prologue + opening] ?? "").trimStart().startsWith("///") &&
        stripComment(all[prologue + opening] ?? "").trim() === ""
    ) {
        opening += 1;
    }
    const first = (all[prologue + opening] ?? "").trimStart().startsWith("///")
        ? stripComment(all[prologue + opening] ?? "").trim()
        : "";
    if (headingLevel(first) === 0 && !ENTRY_RE.test(first)) {
        return header;
    }
    header.headerStart = prologue;
    // The headings and the blank lines around them.
    let headingsEnd = 0;
    for (let line = comment(); line !== null; line = comment()) {
        const text = line.trim();
        if (headingLevel(text) > 0) {
            header.headings.push(text);
            header.headingLines.push(i + 1);
            headingsEnd = i + 1;
        } else if (text !== "") {
            break;
        }
        i += 1;
    }
    const firstHeading = header.headings[0];
    if (firstHeading !== undefined && headingLevel(firstHeading) === 1) {
        header.name = firstHeading.slice(1).trim();
    }
    const titleIndex = header.headings.findIndex((heading) => headingLevel(heading) === 1);
    const titleLine = header.headingLines[titleIndex];
    if (titleLine !== undefined) {
        const separator = all[titleLine] ?? "";
        header.titleSeparated =
            separator.trimStart().startsWith("///") && stripComment(separator).trim() === "";
    }
    // No list: when the line after the headings' blank separator is not an
    // entry attempt, the description starts at that separator. (Without a
    // separator the line is in list position, and malformed.)
    const next = comment();
    if (next !== null && i > headingsEnd && !ENTRY_RE.test(next.trim())) {
        i = headingsEnd;
    }
    // The metadata list, up to its blank separator. Any `- something:` here
    // is an entry attempt (a misspelled key must error), anything else is
    // malformed.
    for (let line = comment(); line !== null && line.trim() !== ""; line = comment()) {
        const match = META_RE.exec(line.trim());
        if (match) {
            header.meta.push({
                key: match[1] ?? "",
                value: (match[2] ?? "").trim(),
                line: i + 1,
            });
        } else {
            header.malformed.push({ text: line.trim(), line: i + 1 });
        }
        i += 1;
    }
    header.proseSeparatorLine = i + 1;
    header.proseSeparated = comment()?.trim() === "";
    const prose: { text: string; line: number }[] = [];
    for (let line = comment(); line !== null; line = comment()) {
        prose.push({ text: line, line: i + 1 });
        i += 1;
    }
    header.bodyStart = i;

    const trimmed = [...prose];
    while (trimmed.length > 0 && (trimmed[0]?.text ?? "").trim() === "") {
        trimmed.shift();
    }
    while (trimmed.length > 0 && (trimmed[trimmed.length - 1]?.text ?? "").trim() === "") {
        trimmed.pop();
    }
    const summaryEnd = trimmed.findIndex(({ text }) => text.trim() === "");
    const split = summaryEnd < 0 ? trimmed.length : summaryEnd;
    header.summaryLines = trimmed.slice(0, split);
    header.summary = header.summaryLines.map(({ text }) => text).join("\n");
    header.descriptionLines = trimmed.slice(split);
    header.description = header.descriptionLines.map(({ text }) => text);

    let notes = header.bodyStart;
    if ((all[notes] ?? "").trim() === "") {
        notes += 1;
    }
    while ((all[notes] ?? "").trimStart().startsWith("// snap:")) {
        header.notes.push((all[notes] ?? "").trimStart().slice("// snap:".length).trimStart());
        notes += 1;
    }
    return header;
}

function problem(filePath: string, line: number, message: string): string {
    return `${filePath}:${line}: ${message}`;
}

const TITLE_RE = /^# (\S(?:.*\S)?)$/;
const TERMINAL_PUNCTUATION_RE = /[.!?;:,]$/;
const SUMMARY_SENTENCE_RE = /[.!?](?:["')\]]+)?(?:\s|$)/;
const MAINTAINER_PROSE_RE =
    /\b(?:fixture|snapshots?|inspect path|server path|harness|pins?|pinned|UPDATE_SNAPSHOTS)\b|(?:verify|snap):/i;

/// Validate the documented-fixture contract. `section` is empty for a
/// corpus-root fixture and names the generated-region directory otherwise.
/// Every diagnostic is ready to print and carries a one-based file line.
export function validateFixtureHeader(
    content: string,
    filePath: string,
    section: string,
): string[] {
    const header = scanFixtureHeader(content);
    const problems: string[] = [];
    const titleIndex = header.headings.findIndex((heading) => headingLevel(heading) === 1);
    const documented = titleIndex >= 0;

    if (!documented) {
        if (section !== "") {
            problems.push(
                problem(filePath, 1, "R7: an undocumented fixture must live at the corpus root"),
            );
        }
        // R7 applies to a fixture prologue, not to ordinary declaration
        // documentation later in the example. A prologue is a leading
        // `///` block separated from the code by a blank line; a doc comment
        // immediately attached to the first declaration is example code.
        let leadingDoc = 0;
        while (leadingDoc < header.lines.length && (header.lines[leadingDoc] ?? "").trim() === "") {
            leadingDoc += 1;
        }
        let afterLeadingDoc = leadingDoc;
        while (
            afterLeadingDoc < header.lines.length &&
            (header.lines[afterLeadingDoc] ?? "").trimStart().startsWith("///")
        ) {
            afterLeadingDoc += 1;
        }
        if (afterLeadingDoc > leadingDoc && (header.lines[afterLeadingDoc] ?? "").trim() === "") {
            problems.push(
                problem(filePath, leadingDoc + 1, "R7: edge-case prologues must use //, not ///"),
            );
        }
        return problems;
    }

    if (header.headerStart !== 0) {
        problems.push(problem(filePath, 1, "R6: the document header must begin on the first line"));
    }

    const title = header.headings[titleIndex] ?? "";
    const titleLine = header.headingLines[titleIndex] ?? header.headerStart + 1;
    if (!TITLE_RE.test(title)) {
        problems.push(problem(filePath, titleLine, "R1: expected '/// # Name'"));
    }
    const name = header.name;
    if (name.includes(" — ")) {
        problems.push(problem(filePath, titleLine, "R1: the title must contain only the name"));
    }
    if (name.split(/\s+/).filter(Boolean).length > 5) {
        problems.push(problem(filePath, titleLine, "R1: the name must contain at most five words"));
    }
    if (TERMINAL_PUNCTUATION_RE.test(name)) {
        problems.push(problem(filePath, titleLine, "R1: the name must not end in punctuation"));
    }
    for (const [index, heading] of header.headings.entries()) {
        if (index !== titleIndex) {
            problems.push(
                problem(
                    filePath,
                    header.headingLines[index] ?? titleLine,
                    `R1: a document header has one heading, found '${heading}'`,
                ),
            );
        }
    }

    if (!header.titleSeparated) {
        problems.push(
            problem(filePath, titleLine + 1, "R2: metadata must follow a blank /// line"),
        );
    }
    for (const malformed of header.malformed) {
        problems.push(
            problem(
                filePath,
                malformed.line,
                `R2: malformed metadata '${malformed.text}' (expected '- key: value')`,
            ),
        );
    }
    const seen = new Set<string>();
    let previous = -1;
    for (const entry of header.meta) {
        const order = FIXTURE_META_KEYS.indexOf(entry.key);
        if (order < 0) {
            problems.push(problem(filePath, entry.line, `R2: unknown metadata key '${entry.key}'`));
        } else if (seen.has(entry.key)) {
            problems.push(
                problem(filePath, entry.line, `R2: duplicate metadata key '${entry.key}'`),
            );
        } else if (order <= previous) {
            problems.push(problem(filePath, entry.line, "R2: metadata keys are out of order"));
        }
        seen.add(entry.key);
        previous = Math.max(previous, order);
    }
    if (header.meta[0]?.key !== "status") {
        problems.push(
            problem(
                filePath,
                header.meta[0]?.line ?? titleLine + 2,
                "R2: status must be the first metadata entry",
            ),
        );
    }
    if (!header.proseSeparated) {
        problems.push(
            problem(filePath, header.proseSeparatorLine, "R3: metadata must be followed by prose"),
        );
    }

    const summary = header.summaryLines.map(({ text }) => text.trim()).join(" ");
    const summaryLine = header.summaryLines[0]?.line ?? header.proseSeparatorLine;
    if (summary === "") {
        problems.push(
            problem(filePath, summaryLine, "R3: the first prose paragraph must be a summary"),
        );
    } else {
        if (/^[a-z]/.test(summary)) {
            problems.push(
                problem(filePath, summaryLine, "R3: the summary must begin with a capital letter"),
            );
        }
        if (TERMINAL_PUNCTUATION_RE.test(summary)) {
            problems.push(
                problem(filePath, summaryLine, "R3: the summary must not end in punctuation"),
            );
        }
        const withoutEnd = summary.replace(/[.!?](?:["')\]]+)?$/, "");
        if (SUMMARY_SENTENCE_RE.test(withoutEnd)) {
            problems.push(problem(filePath, summaryLine, "R3: the summary must be one sentence"));
        }
    }
    for (const proseLine of [...header.summaryLines, ...header.descriptionLines]) {
        if (MAINTAINER_PROSE_RE.test(proseLine.text)) {
            problems.push(
                problem(
                    filePath,
                    proseLine.line,
                    "R4: reader-facing prose contains maintainer terminology",
                ),
            );
        }
    }

    const snapLines = header.lines
        .map((line, index) => ({ index, line: index + 1, text: line.trimStart() }))
        .filter(({ text }) => text.startsWith("// snap:"));
    if (snapLines.length > 0) {
        const expected = header.bodyStart + 1;
        if (
            (header.lines[header.bodyStart] ?? "").trim() !== "" ||
            snapLines[0]?.index !== expected
        ) {
            problems.push(
                problem(
                    filePath,
                    snapLines[0]?.line ?? header.bodyStart + 1,
                    "R5: // snap: must follow the header after one blank line",
                ),
            );
        }
        let note = snapLines[0]?.index ?? 0;
        while ((header.lines[note] ?? "").trimStart().startsWith("//")) {
            if (!(header.lines[note] ?? "").trimStart().startsWith("// snap:")) {
                problems.push(
                    problem(
                        filePath,
                        note + 1,
                        "R5: every snapshot-note line must begin with // snap:",
                    ),
                );
            }
            note += 1;
        }
        for (const snap of snapLines.slice(1)) {
            if (snap.index >= note) {
                problems.push(
                    problem(
                        filePath,
                        snap.line,
                        "R5: // snap: blocks may appear only after the header",
                    ),
                );
            }
        }
    }
    return problems;
}

export function parseFixtureMeta(content: string, filePath: string): FixtureMeta {
    const meta: FixtureMeta = {
        status: "supported",
        verify: "both",
        snap: "shared",
        diagnostics: false,
        indexing: false,
        flags: [],
    };

    const header = scanFixtureHeader(content);
    for (const line of header.malformed) {
        throw new Error(`${filePath}: malformed fixture meta line '${line.text}'`);
    }
    // A repeated key (merge leftovers, copy/paste) must not silently
    // let the later value win — it could flip a snap mode unnoticed.
    const seen = new Set<string>();
    for (const { key, value } of header.meta) {
        if (!FIXTURE_META_KEYS.includes(key)) {
            throw new Error(`${filePath}: unknown fixture meta key '${key}'`);
        }
        if (seen.has(key)) {
            throw new Error(`${filePath}: duplicate fixture meta key '${key}'`);
        }
        seen.add(key);
        if (key === "status") {
            if (value !== "supported" && value !== "partial" && value !== "unsupported") {
                throw new Error(`${filePath}: invalid status '${value}'`);
            }
            meta.status = value;
        } else if (key === "verify") {
            if (value !== "both" && value !== "inspect" && value !== "server") {
                throw new Error(`${filePath}: invalid verify mode '${value}'`);
            }
            meta.verify = value;
        } else if (key === "snap") {
            if (value !== "shared" && value !== "separate" && value !== "skip") {
                throw new Error(`${filePath}: invalid snap mode '${value}'`);
            }
            meta.snap = value;
        } else if (key === "config") {
            let parsed: unknown;
            try {
                parsed = JSON.parse(value);
            } catch {
                throw new Error(`${filePath}: config is not valid JSON: ${value}`);
            }
            if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
                throw new Error(`${filePath}: config must be a JSON object`);
            }
            meta.config = value;
        } else if (key === "diagnostics") {
            if (value !== "expected") {
                throw new Error(`${filePath}: invalid diagnostics value '${value}'`);
            }
            meta.diagnostics = true;
        } else if (key === "indexing") {
            if (value !== "true" && value !== "false") {
                throw new Error(`${filePath}: invalid indexing value '${value}'`);
            }
            meta.indexing = value === "true";
        } else if (key === "flags") {
            let parsed: unknown;
            try {
                parsed = JSON.parse(value);
            } catch {
                throw new Error(`${filePath}: flags is not a JSON array: ${value}`);
            }
            if (!Array.isArray(parsed) || !parsed.every((flag) => typeof flag === "string")) {
                throw new Error(`${filePath}: flags must be a JSON string array`);
            }
            meta.flags = parsed;
        }
    }
    // The relation between the two paths only exists when both run.
    if (seen.has("snap") && meta.verify !== "both") {
        throw new Error(`${filePath}: snap: ${meta.snap} requires verify: both`);
    }
    return meta;
}

export interface FixtureFile {
    /// Corpus-relative POSIX path.
    rel: string;
    /// Raw file text, annotations included.
    content: string;
    source: AnnotatedSource;
}

export interface SnapFixture {
    /// Corpus-relative path of the entry file (the fixture source itself,
    /// or `<unit>/main.cpp`).
    rel: string;
    /// Unit directory rel; "" for a single-file fixture.
    unit: string;
    /// The section directory the fixture lives in; "" at the corpus root.
    section: string;
    meta: FixtureMeta;
    /// The fixture's C-family sources: just the entry for a single-file
    /// fixture, entry plus siblings for a unit.
    files: FixtureFile[];
    /// Non-source unit files, copied verbatim into materialized workspaces.
    extras: string[];
    /// False for `status: unsupported` and `snap: skip` fixtures, which
    /// run nowhere and keep no snapshot.
    active: boolean;
}

/// A fixture file's name relative to the fixture itself: unit-relative for
/// a unit's files, the bare file name for a single-file fixture — what
/// `clice inspect` keys its entries by and what snapshot sections are
/// labeled with, unchanged by the section directory the fixture lives in.
export function fixtureRelative(fixture: SnapFixture, file: FixtureFile): string {
    return fixture.unit === ""
        ? path.posix.basename(file.rel)
        : file.rel.slice(fixture.unit.length + 1);
}

export interface SnapCorpus {
    feature: string;
    corpus: string;
    /// corpus.json manifest flags, `${corpus}` still unresolved.
    flags: string[];
    /// The config section `config:` overlays target on the server (the
    /// feature name unless the manifest overrides it — the inlay_hint
    /// corpus configures the `[inlay_hints]` section).
    configSection: string;
    fixtures: SnapFixture[];
    /// Corpus-root support entries shared by every fixture (include roots,
    /// `.clang-format`, ...), as corpus-relative paths.
    support: string[];
}

export const C_FAMILY = /\.(cpp|cc|cxx|c|cppm|h|hpp|hh)$/;
const COMPILABLE = /\.(cpp|cppm)$/;
export const HEADER = /\.(h|hpp|hh)$/;

/// Substitute `${corpus}` with the root the flags run against: the corpus
/// directory on the inspect path, the materialized workspace root on the
/// server path.
export function resolveFlags(flags: string[], root: string): string[] {
    const posix = root.split(path.sep).join("/");
    return flags.map((flag) => flag.replaceAll("${corpus}", posix));
}

function readManifest(feature: string, corpus: string): { flags: string[]; configSection: string } {
    const file = path.join(corpus, "corpus.json");
    if (!fs.existsSync(file)) {
        return { flags: ["-std=c++20"], configSection: feature };
    }
    const manifest: unknown = JSON.parse(fs.readFileSync(file, "utf8"));
    if (typeof manifest !== "object" || manifest === null || Array.isArray(manifest)) {
        throw new Error(`${file}: manifest must be a JSON object`);
    }
    // `notes` carries the why of the flags — JSON has no comments.
    for (const key of Object.keys(manifest)) {
        if (key !== "flags" && key !== "config_section" && key !== "notes") {
            throw new Error(`${file}: unknown manifest key '${key}'`);
        }
    }
    const { flags, config_section } = manifest as { flags?: unknown; config_section?: unknown };
    if (!Array.isArray(flags) || !flags.every((flag) => typeof flag === "string")) {
        throw new Error(`${file}: flags must be a JSON string array`);
    }
    if (config_section !== undefined && typeof config_section !== "string") {
        throw new Error(`${file}: config_section must be a string`);
    }
    return { flags, configSection: config_section ?? feature };
}

/// Enumerate the corpora under `root` (tests/snap).
export function snapCorpora(root = SNAP_DIR): SnapCorpus[] {
    const corpora: SnapCorpus[] = [];
    for (const feature of fs.readdirSync(root).sort()) {
        const corpus = path.join(root, feature);
        if (!fs.statSync(corpus).isDirectory()) {
            continue;
        }
        const entries = fs
            .readdirSync(corpus, { recursive: true, encoding: "utf8" })
            .map((name) => name.split(path.sep).join("/"))
            .filter((rel) => fs.statSync(path.join(corpus, rel)).isFile())
            .filter(
                (rel) =>
                    !rel.endsWith(".snap.yml") &&
                    !rel.endsWith(".snap.yml.new") &&
                    path.basename(rel) !== "compile_commands.json" &&
                    rel !== "corpus.json",
            )
            .sort();

        const units = entries
            .filter((rel) => rel.endsWith("/main.cpp"))
            .map((rel) => rel.slice(0, -"/main.cpp".length));
        for (const unit of units) {
            if (units.some((other) => other !== unit && other.startsWith(`${unit}/`))) {
                throw new Error(`tests/snap/${feature}/${unit}: nested fixture units`);
            }
            if (unit.split("/").length > 2) {
                throw new Error(
                    `tests/snap/${feature}/${unit}: a unit lives at the corpus root or in a ` +
                        "section directory",
                );
            }
        }
        const sectionOf = (rel: string): string => {
            const parts = rel.split("/");
            return parts.length === 2 ? (parts[0] ?? "") : "";
        };
        const owningUnit = (rel: string): string | undefined =>
            units.find((unit) => rel.startsWith(`${unit}/`));

        const support: string[] = [];
        const fixtures: SnapFixture[] = [];
        const unitFiles = new Map<string, string[]>(units.map((unit) => [unit, []]));
        for (const rel of entries) {
            const unit = owningUnit(rel);
            if (unit !== undefined) {
                unitFiles.get(unit)?.push(rel);
                continue;
            }
            if (rel.endsWith(".cpp")) {
                if (rel.split("/").length > 2) {
                    throw new Error(
                        `tests/snap/${feature}/${rel}: fixture sources live at the corpus ` +
                            "root, in a section directory, or in a main.cpp unit",
                    );
                }
                fixtures.push(makeFixture(corpus, feature, rel, "", sectionOf(rel), [rel], []));
                continue;
            }
            // Support sources reach the inspect path unstripped (pulled in
            // via include search from the real corpus), so a marker in one
            // would silently diverge the two paths.
            if (C_FAMILY.test(rel)) {
                const content = fs.readFileSync(path.join(corpus, rel), "utf8");
                if (parseAnnotations(content).content !== content) {
                    throw new Error(
                        `tests/snap/${feature}/${rel}: support files cannot carry §-markers`,
                    );
                }
            }
            support.push(rel);
        }
        for (const [unit, rels] of unitFiles) {
            const sources = rels.filter((rel) => C_FAMILY.test(rel));
            const extras = rels.filter((rel) => !C_FAMILY.test(rel));
            fixtures.push(
                makeFixture(
                    corpus,
                    feature,
                    `${unit}/main.cpp`,
                    unit,
                    sectionOf(unit),
                    sources,
                    extras,
                ),
            );
        }
        fixtures.sort((a, b) => (a.rel < b.rel ? -1 : a.rel > b.rel ? 1 : 0));

        const manifest = readManifest(feature, corpus);
        corpora.push({
            feature,
            corpus,
            flags: manifest.flags,
            configSection: manifest.configSection,
            fixtures,
            support,
        });
    }
    return corpora;
}

function makeFixture(
    corpus: string,
    feature: string,
    rel: string,
    unit: string,
    section: string,
    sources: string[],
    extras: string[],
): SnapFixture {
    const files = sources.map((sourceRel) => {
        const content = fs.readFileSync(path.join(corpus, sourceRel), "utf8");
        return { rel: sourceRel, content, source: parseAnnotations(content) };
    });
    const entry = files.find((file) => file.rel === rel);
    if (!entry) {
        throw new Error(`tests/snap/${feature}/${rel}: missing entry source`);
    }
    const meta = parseFixtureMeta(entry.content, `${feature}/${rel}`);
    const active = meta.status !== "unsupported" && meta.snap !== "skip";
    return { rel, unit, section, meta, files, extras, active };
}

/// Write one fixture's view of the corpus into `root`: support files
/// verbatim (enumeration rejects markers in them) and unit sources with
/// annotations stripped, so the server compiles from disk exactly what
/// the inspect path compiles from memory — plus a compile_commands.json
/// built from the manifest and fixture flags.
export function materializeFixture(corpus: SnapCorpus, fixture: SnapFixture, root: string): void {
    const write = (rel: string, content: string | Buffer): void => {
        const target = path.join(root, rel);
        fs.mkdirSync(path.dirname(target), { recursive: true });
        fs.writeFileSync(target, content);
    };
    for (const rel of corpus.support) {
        write(rel, fs.readFileSync(path.join(corpus.corpus, rel)));
    }
    for (const file of fixture.files) {
        write(file.rel, file.source.content);
    }
    for (const rel of fixture.extras) {
        write(rel, fs.readFileSync(path.join(corpus.corpus, rel)));
    }

    const flags = [...resolveFlags(corpus.flags, root), ...resolveFlags(fixture.meta.flags, root)];
    const posixRoot = root.split(path.sep).join("/");
    // Fixture sources compile from the fixture's own directory (the unit
    // directory, else the section directory or the root), mirroring the
    // input directory of the inspect path, so relative compiler operands
    // (-Iinclude, @args.rsp, ...) resolve identically on both paths.
    // Support sources are never inspected; their cwd is where they live.
    const own = fixture.unit === "" ? fixture.section : fixture.unit;
    const unitDir = own === "" ? posixRoot : `${posixRoot}/${own}`;
    const unitRels = new Set(fixture.files.map((file) => file.rel));
    const compilable = [
        ...fixture.files.map((file) => file.rel),
        ...corpus.support.filter((rel) => COMPILABLE.test(rel)),
    ].sort();
    write(
        "compile_commands.json",
        JSON.stringify(
            compilable.map((rel) => ({
                directory: unitRels.has(rel) ? unitDir : posixRoot,
                file: `${posixRoot}/${rel}`,
                // Mirror the inspect path's driver choice (file_command):
                // C sources take the C driver, everything else (C++,
                // headers) the C++ one.
                arguments: [
                    rel.endsWith(".c") ? "clang" : "clang++",
                    ...flags,
                    "-fsyntax-only",
                    `${posixRoot}/${rel}`,
                ],
            })),
            null,
            2,
        ),
    );
}

/// Snapshots follow their sources: a stale `.snap.yml` whose fixture was
/// renamed, deleted, marked unsupported/skip — or a variant left behind
/// after a fixture changed verify/snap mode — must not linger as if it
/// still pinned anything.
export function orphanSnapshots({ corpus, fixtures }: SnapCorpus): string[] {
    const allowed = new Set<string>();
    for (const fixture of fixtures) {
        if (!fixture.active) {
            continue;
        }
        const base = fixture.rel.replace(/\.cpp$/, "");
        if (fixture.meta.verify === "both" && fixture.meta.snap === "separate") {
            allowed.add(`${base}.inspect.snap.yml`);
            allowed.add(`${base}.server.snap.yml`);
        } else {
            allowed.add(`${base}.snap.yml`);
        }
    }
    return fs
        .readdirSync(corpus, { recursive: true, encoding: "utf8" })
        .filter((name) => name.endsWith(".snap.yml"))
        .map((name) => name.split(path.sep).join("/"))
        .filter((rel) => !allowed.has(rel));
}
