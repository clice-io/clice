/// Generate feature-doc checklist sections from snapshot fixtures.
///
/// Each fixture .cpp under tests/data/<feature>/ may begin with a doc header
/// describing one checklist capability. This tool renders those headers into
/// the GENERATED regions of docs/en/features/*.md, so the fixtures are the
/// single source of truth and the doc checklist is derived from them.
///
/// Fixture doc header:
///
///     /// # Fold Kinds
///     ///
///     /// ## Block folding — functions, classes, ...
///     ///
///     /// - status: supported
///     /// - issues: clangd#1455, vscode#70794
///     /// - order: 1
///     ///
///     /// Optional markdown description after a bare `///` separator.
///
/// A file is a doc item iff its first line (after stripping `/// `) starts
/// with `# `. Anything else is a supplementary edge-case test, excluded
/// from docs. The heading hierarchy is plain markdown: with an `## item
/// title` under the h1, the h1 names the doc section the item belongs to —
/// matched verbatim against the doc page's generated-region key
/// (`<!-- BEGIN GENERATED ITEMS: Fold Kinds -->`); an h1 alone is the item
/// title, with the fixture's subdirectory as the legacy section fallback.
/// A blank `///` separates the headings from a metadata list of
/// `/// - key: value` lines; the known keys are `status` (required;
/// `supported`, `partial` or `unsupported`), `issues` (optional), `order`
/// (optional integer) and `snap` (snapshot suites, not rendered). A bare
/// `///` then separates the metadata from an optional markdown
/// description; everything after the last `///` line (trimmed of blank
/// lines) is the example code.
///
/// `partial` items render unchecked with a _(partial)_ marker but are still
/// compiled and snapshotted, so the snapshot records the current partial
/// behavior; only `unsupported` fixtures are skipped by the snapshot glob (via
/// test/fixture.h, which reads the same header).
///
/// Usage:
///     node tools/feature_docs.ts update   # rewrite generated regions
///     node tools/feature_docs.ts check    # fail if regions are stale

import * as fs from "node:fs";
import * as path from "node:path";
import { REPO_ROOT } from "./compile_commands.ts";
import { parseAnnotations } from "./snap/annotation.ts";

// feature -> doc path (relative to repo root). Extend as more features
// adopt fixture-generated docs.
const FEATURES: Record<string, string> = {
    code_completion: "docs/en/features/completion.md",
    document_symbol: "docs/en/features/document-symbols.md",
    folding_range: "docs/en/features/folding-ranges.md",
    inlay_hint: "docs/en/features/inlay-hints.md",
    semantic_tokens: "docs/en/features/semantic-tokens.md",
    signature_help: "docs/en/features/signature-help.md",
};

const ISSUE_TRACKERS: Record<string, string> = {
    clangd: "https://github.com/clangd/clangd/issues/",
    vscode: "https://github.com/microsoft/vscode/issues/",
    llvm: "https://github.com/llvm/llvm-project/issues/",
};

// `snap` and `config` are consumed by the snapshot suites
// (tools/snap/inspect.ts), not rendered into docs.
const KNOWN_KEYS: readonly string[] = [
    "status",
    "issues",
    "order",
    "verify",
    "snap",
    "config",
    "diagnostics",
    "indexing",
    "flags",
];
const VALID_STATUS: readonly string[] = ["supported", "partial", "unsupported"];

// Markers must occupy their own unindented line, so marker text embedded in
// generated item content (titles, descriptions, example code) can never
// open or terminate a region.
const BEGIN_RE = /^<!-- BEGIN GENERATED ITEMS: (.+?) -->$/;
const END_MARKER = "<!-- END GENERATED ITEMS -->";
const ISSUE_RE = /^([a-z]+)#(\d+)$/;
// A metadata list entry: `- key: value`.
const META_RE = /^-\s+(\w+):\s*(.*)$/;

interface Fixture {
    path: string;
    section: string;
    title: string;
    status: string;
    issues: string[];
    order: number | null;
    description: string;
    example: string;
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

/// Return the text of a `///` comment line, minus prefix and one space.
function stripComment(line: string): string {
    let text = line.trimStart().slice(3);
    if (text.startsWith(" ")) {
        text = text.slice(1);
    }
    return text;
}

function trimBlank(lines: string[]): string[] {
    const result = [...lines];
    while (result.length > 0 && (result[0] ?? "").trim() === "") {
        result.shift();
    }
    while (result.length > 0 && (result[result.length - 1] ?? "").trim() === "") {
        result.pop();
    }
    return result;
}

/// Parse a Python-style base-10 integer (optional sign, digit-group
/// underscores), matching int() so `order` values round-trip identically.
function parseIntStrict(value: string): number | null {
    const t = value.trim();
    if (!/^[+-]?\d+(?:_\d+)*$/.test(t)) {
        return null;
    }
    return Number(t.replaceAll("_", ""));
}

/// Parse a fixture's doc header. Returns null for supplementary files.
function parseFixture(filePath: string, featureDir: string, problems: string[]): Fixture | null {
    const text = fs.readFileSync(filePath, "utf8");
    const lines = splitLines(text);

    /// Stripped `///` text at line i, or null if it is not a `///` line.
    const comment = (i: number): string | null => {
        const raw = lines[i];
        if (!raw?.trimStart().startsWith("///")) {
            return null;
        }
        return stripComment(raw);
    };

    const first = comment(0);
    if (!first?.startsWith("# ")) {
        // Not an h1 line: supplementary fixture, not a doc item.
        return null;
    }

    const relParts = path.relative(featureDir, filePath).split(path.sep);
    if (relParts.length > 2) {
        problems.push(
            `${filePath}: doc-item fixture must be at most one subdirectory deep ` +
                `(found '${relParts.join("/")}')`,
        );
    }

    // The heading hierarchy is plain markdown: an `## item title` after the
    // h1 makes the h1 the section (matched verbatim against the doc page's
    // `<!-- BEGIN GENERATED ITEMS: ... -->` key); an h1 alone is the item
    // title with the fixture's subdirectory as the legacy section fallback.
    let i = 1;
    if (comment(i) === "") {
        i += 1;
    }
    let section = "";
    let title = "";
    const second = comment(i);
    if (second?.startsWith("## ")) {
        section = first.slice(2).trim();
        title = second.slice(3).trim();
        i += 1;
        if (comment(i) === "") {
            i += 1;
        }
    } else {
        title = first.slice(2).trim();
        section = relParts.length >= 2 ? (relParts[0] ?? "") : "";
    }
    if (!title) {
        problems.push(`${filePath}: empty title`);
    }
    if (!section) {
        problems.push(
            `${filePath}: doc-item fixture needs an '# section' heading above its ` +
                "'## title' (or a section subdirectory)",
        );
    }

    const keys = new Map<string, string>();
    for (;;) {
        const line = comment(i);
        if (line === null || line.trim() === "") {
            break;
        }
        const stripped = line.trim();
        const match = META_RE.exec(stripped);
        if (!match) {
            problems.push(
                `${filePath}: malformed metadata line '${stripped}' ` +
                    "(expected '- key: value'; separate the description with a bare ///)",
            );
            i += 1;
            continue;
        }
        const key = match[1] ?? "";
        if (!KNOWN_KEYS.includes(key)) {
            problems.push(`${filePath}: unknown key '${key}'`);
        } else if (keys.has(key)) {
            problems.push(`${filePath}: duplicate ${key}`);
        }
        keys.set(key, (match[2] ?? "").trim());
        i += 1;
    }

    // Everything from the trailing bare `///` up to the first non-comment
    // line is the markdown description.
    const desc: string[] = [];
    for (;;) {
        const line = comment(i);
        if (line === null) {
            break;
        }
        desc.push(line);
        i += 1;
    }
    const bodyStart = i;

    if (!keys.has("status")) {
        problems.push(`${filePath}: missing required key 'status'`);
    } else if (!keys.get("status")) {
        problems.push(`${filePath}: empty key 'status'`);
    }
    const status = keys.get("status") ?? "";
    if (keys.has("status") && status && !VALID_STATUS.includes(status)) {
        problems.push(
            `${filePath}: invalid status '${status}' (expected one of ${VALID_STATUS.join(", ")})`,
        );
    }

    const issues: string[] = [];
    for (const rawRef of (keys.get("issues") ?? "").split(",")) {
        const ref = rawRef.trim();
        if (!ref) {
            continue;
        }
        const match = ISSUE_RE.exec(ref);
        const tracker = match?.[1] ?? "";
        if (!match || !Object.hasOwn(ISSUE_TRACKERS, tracker)) {
            problems.push(`${filePath}: unknown issue reference '${ref}'`);
            continue;
        }
        issues.push(ref);
    }

    let order: number | null = null;
    if (keys.has("order")) {
        const raw = keys.get("order") ?? "";
        const parsed = parseIntStrict(raw);
        if (parsed === null) {
            problems.push(`${filePath}: order must be an integer, got '${raw}'`);
        } else {
            order = parsed;
        }
    }

    // A plain `//` comment block opening with `// snap:` directly after the
    // header explains the fixture's snapshot mode to maintainers; it is not
    // part of the rendered example code. (A bare leading `//` comment stays:
    // e.g. the comment-folding example is itself a comment.)
    let exampleStart = bodyStart;
    while ((lines[exampleStart] ?? "").trim() === "" && exampleStart < lines.length) {
        exampleStart += 1;
    }
    if ((lines[exampleStart] ?? "").trim().startsWith("// snap:")) {
        while ((lines[exampleStart] ?? "").trim().startsWith("//")) {
            exampleStart += 1;
        }
    }
    // Snapshot-focus `§` markers are fixture metadata, not example code.
    const example = parseAnnotations(trimBlank(lines.slice(exampleStart)).join("\n")).content;
    if (!example.trim()) {
        problems.push(`${filePath}: doc-item fixture has no example code`);
    }

    return {
        path: filePath,
        section,
        title,
        status,
        issues,
        order,
        description: trimBlank(desc).join("\n"),
        example,
    };
}

function renderIssue(ref: string): string {
    const hash = ref.indexOf("#");
    const tracker = ref.slice(0, hash);
    const number = ref.slice(hash + 1);
    return `[${ref}](${ISSUE_TRACKERS[tracker] ?? ""}${number})`;
}

function indent(text: string, prefix = "  "): string[] {
    return text.split("\n").map((line) => `${prefix}${line}`.trimEnd());
}

function renderItem(fx: Fixture): string {
    const box = fx.status === "supported" ? "[x]" : "[ ]";
    let line = `- ${box} ${fx.title}`;
    // Underscore emphasis matches prettier's markdown style, so `pixi run
    // format` leaves the generated regions untouched.
    if (fx.status === "partial") {
        line += " _(partial)_";
    }
    if (fx.issues.length > 0) {
        line += ` (${fx.issues.map(renderIssue).join(", ")})`;
    }

    const out = [line];
    if (fx.description) {
        out.push("");
        out.push(...indent(fx.description));
    }
    if (fx.example) {
        // A fence longer than any backtick run in the example, so example
        // code can never close the fence early.
        const runs = fx.example.match(/`+/g) ?? [];
        const longest = runs.reduce((max, run) => Math.max(max, run.length), 0);
        const fence = "`".repeat(Math.max(3, longest + 1));
        // `<details>` rather than VitePress's `::: details` container so the
        // example collapses on GitHub too.
        out.push("");
        out.push("  <details>");
        out.push("  <summary>Example</summary>");
        out.push("");
        out.push(`  ${fence}cpp`);
        out.push(...indent(fx.example));
        out.push(`  ${fence}`);
        out.push("");
        out.push("  </details>");
    }
    return out.join("\n");
}

function collectFixtures(feature: string, problems: string[]): Fixture[] {
    // Snapshot corpora migrated to tests/snap/ keep feeding the docs from
    // their new home; the rest still live under tests/data/.
    const snapDir = path.join(REPO_ROOT, "tests", "snap", feature);
    const dataDir = fs.existsSync(snapDir)
        ? snapDir
        : path.join(REPO_ROOT, "tests", "data", feature);
    const fixtures: Fixture[] = [];
    const titles = new Map<string, string>();
    for (const filePath of globCpp(dataDir)) {
        const fx = parseFixture(filePath, dataDir, problems);
        if (fx === null) {
            continue;
        }
        const prev = titles.get(fx.title);
        if (prev !== undefined) {
            problems.push(`${filePath}: duplicate title '${fx.title}' (also in ${prev})`);
        } else {
            titles.set(fx.title, filePath);
        }
        fixtures.push(fx);
    }
    fixtures.sort((a, b) => {
        const oa = a.order ?? 1 << 30;
        const ob = b.order ?? 1 << 30;
        if (oa !== ob) {
            return oa - ob;
        }
        const na = path.basename(a.path);
        const nb = path.basename(b.path);
        return na < nb ? -1 : na > nb ? 1 : 0;
    });
    return fixtures;
}

/// All *.cpp under dir at any depth, sorted by full path (matches
/// sorted(Path.glob("**/*.cpp"))).
function globCpp(dir: string): string[] {
    if (!fs.existsSync(dir)) {
        return [];
    }
    return fs
        .readdirSync(dir, { recursive: true, encoding: "utf8" })
        .filter((name) => name.endsWith(".cpp"))
        .map((name) => path.join(dir, name))
        .sort();
}

function renderRegion(section: string, fixtures: Fixture[]): string {
    return fixtures
        .filter((fx) => fx.section === section)
        .map(renderItem)
        .join("\n\n");
}

function rewriteDoc(
    docText: string,
    sections: Map<string, Fixture[]>,
    docPath: string,
    problems: string[],
): string {
    const lines = docText.split("\n");
    const out: string[] = [];
    const docSections = new Set<string>();

    let idx = 0;
    while (idx < lines.length) {
        const line = lines[idx] ?? "";
        const match = BEGIN_RE.exec(line);
        if (!match) {
            out.push(line);
            idx += 1;
            continue;
        }

        const section = (match[1] ?? "").trim();
        if (docSections.has(section)) {
            problems.push(`${docPath}: duplicate region '${section}'`);
        }
        docSections.add(section);
        let end = idx + 1;
        while (end < lines.length && lines[end] !== END_MARKER) {
            end += 1;
        }
        if (end >= lines.length) {
            problems.push(`${docPath}: region '${section}' has no closing marker`);
            for (let k = idx; k < lines.length; k++) {
                out.push(lines[k] ?? "");
            }
            return out.join("\n");
        }

        const matched = sections.get(section) ?? [];
        if (matched.length === 0) {
            problems.push(`${docPath}: region '${section}' matches no fixtures`);
        }

        out.push(line);
        const content = renderRegion(section, matched);
        out.push("");
        if (content) {
            out.push(content);
            out.push("");
        }
        out.push(lines[end] ?? "");
        idx = end + 1;
    }

    for (const section of sections.keys()) {
        if (!docSections.has(section)) {
            problems.push(`${docPath}: section '${section}' has no matching marker region`);
        }
    }

    return out.join("\n");
}

function processFeature(
    feature: string,
    docRel: string,
    problems: string[],
): [string, string, string] {
    const docPath = path.join(REPO_ROOT, docRel);
    const fixtures = collectFixtures(feature, problems);

    const sections = new Map<string, Fixture[]>();
    for (const fx of fixtures) {
        const list = sections.get(fx.section);
        if (list) {
            list.push(fx);
        } else {
            sections.set(fx.section, [fx]);
        }
    }

    // Windows runners check out docs as CRLF (only tests/data/** is pinned
    // to LF); normalize so the $-anchored marker regexes match.
    const current = fs.readFileSync(docPath, "utf8").replaceAll("\r\n", "\n");
    const updated = rewriteDoc(current, sections, docPath, problems);
    return [docPath, current, updated];
}

/// A compact unified-style diff of the differing lines, to report staleness.
function unifiedDiff(current: string, updated: string, fromFile: string, toFile: string): string {
    const a = current.split("\n");
    const b = updated.split("\n");
    const n = a.length;
    const m = b.length;
    const dp: number[][] = [];
    for (let i = 0; i <= n; i++) {
        dp.push(new Array<number>(m + 1).fill(0));
    }
    for (let i = n - 1; i >= 0; i--) {
        const cur = dp[i] ?? [];
        const below = dp[i + 1] ?? [];
        for (let j = m - 1; j >= 0; j--) {
            cur[j] =
                a[i] === b[j] ? (below[j + 1] ?? 0) + 1 : Math.max(below[j] ?? 0, cur[j + 1] ?? 0);
        }
    }
    const out: string[] = [`--- ${fromFile}`, `+++ ${toFile}`];
    let i = 0;
    let j = 0;
    while (i < n && j < m) {
        if (a[i] === b[j]) {
            i += 1;
            j += 1;
        } else if ((dp[i + 1]?.[j] ?? 0) >= (dp[i]?.[j + 1] ?? 0)) {
            out.push(`-${a[i] ?? ""}`);
            i += 1;
        } else {
            out.push(`+${b[j] ?? ""}`);
            j += 1;
        }
    }
    while (i < n) {
        out.push(`-${a[i] ?? ""}`);
        i += 1;
    }
    while (j < m) {
        out.push(`+${b[j] ?? ""}`);
        j += 1;
    }
    return out.join("\n");
}

function main(argv: string[]): number {
    const mode = argv[0];
    if (mode !== "update" && mode !== "check") {
        console.error("usage: feature_docs.ts update|check");
        return 2;
    }

    const problems: string[] = [];
    const results = Object.entries(FEATURES).map(([f, d]) => processFeature(f, d, problems));

    if (problems.length > 0) {
        console.error("feature_docs: problems found:");
        for (const problem of problems) {
            console.error(`  - ${problem}`);
        }
        return 1;
    }

    let stale = false;
    for (const [docPath, current, updated] of results) {
        if (current === updated) {
            continue;
        }
        stale = true;
        if (mode === "update") {
            fs.writeFileSync(docPath, updated, "utf8");
            console.log(`updated ${path.relative(REPO_ROOT, docPath)}`);
        } else {
            const rel = path.relative(REPO_ROOT, docPath);
            console.error(unifiedDiff(current, updated, `${rel} (current)`, `${rel} (generated)`));
        }
    }

    if (mode === "check" && stale) {
        console.error("feature_docs: docs are stale; run 'feature_docs.ts update'");
        return 1;
    }
    return 0;
}

process.exit(main(process.argv.slice(2)));
