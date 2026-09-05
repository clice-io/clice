/// Generate the feature pages' capability sections from snapshot fixtures.
///
/// A fixture .cpp under tests/snap/<feature>/<section>/ may begin with a doc
/// header describing one capability. This tool renders those headers into
/// the GENERATED regions of docs/en/features/*.md, so the fixtures are the
/// single source of truth and the pages are derived from them.
///
/// Fixture doc header:
///
///     /// # Block folding
///     ///
///     /// - status: supported
///     /// - issues: clangd#1455, vscode#70794
///     ///
///     /// Functions, classes, and other blocks produce folding ranges
///
///     // snap: Optional notes about snapshot mechanics go here.
///
/// The header's shape (the heading, fixed-order metadata list, summary,
/// further description, and optional snapshot notes) is read by
/// scanFixtureHeader in
/// tools/snap/corpus.ts, which the snap suite shares; the keys it may
/// carry are FIXTURE_META_KEYS there. A file is a doc item iff the header
/// opens with an h1 (`#`) heading naming the capability; anything else is
/// a supplementary edge-case test, excluded from docs. Where the item
/// renders comes from its path: the section directory it lives in is the
/// doc page's generated-region key (`<!-- BEGIN GENERATED ITEMS:
/// fold_kinds -->`), and the numbered file name (`NN_name.cpp`, or
/// `NN_unit/main.cpp` for a multi-file unit) orders it within the
/// section. This tool renders `status` (required; `supported`, `partial`
/// or `unsupported`) and `issues`; the other keys drive the snapshot
/// suites. Everything after the last `///` line (trimmed of blank lines)
/// is the example code.
///
/// A section renders as a status table of its items followed by one `###`
/// heading per item with its description and example code.
///
/// Usage:
///     node tools/docs/feature.ts update   # rewrite generated regions
///     node tools/docs/feature.ts check    # fail if regions are stale

import * as fs from "node:fs";
import * as path from "node:path";
import { REPO_ROOT } from "../compile_commands.ts";
import { parseAnnotations } from "../snap/annotation.ts";
import {
    FIXTURE_META_KEYS,
    headingLevel,
    scanFixtureHeader,
    snapCorpora,
    validateFixtureHeader,
} from "../snap/corpus.ts";
import { renderMarkdownTable, rewriteRegions, type RegionMarkers } from "./generated.ts";

// feature -> doc path (relative to repo root). Extend as more features
// adopt fixture-generated docs. Several corpora may feed one doc page
// (navigation.md aggregates the navigation and workspace_symbol corpora);
// their fixtures must then use disjoint section keys.
const FEATURES: Record<string, string> = {
    code_completion: "docs/en/features/completion.md",
    document_links: "docs/en/features/document-links.md",
    document_symbol: "docs/en/features/document-symbols.md",
    hover: "docs/en/features/hover.md",
    folding_range: "docs/en/features/folding-ranges.md",
    inlay_hint: "docs/en/features/inlay-hints.md",
    navigation: "docs/en/features/navigation.md",
    semantic_tokens: "docs/en/features/semantic-tokens.md",
    signature_help: "docs/en/features/signature-help.md",
    workspace_symbol: "docs/en/features/navigation.md",
};

/// Rows of the overview status matrix, in display order. `keys` names the
/// corpora in FEATURES whose fixture statuses are aggregated into the row;
/// a row without `keys` is not fixture-backed yet and keeps the
/// hand-assigned label from before the feature joined the pipeline.
const OVERVIEW_ROWS: { name: string; page: string; keys?: string[]; label?: string }[] = [
    { name: "Code Completion", page: "completion", keys: ["code_completion"] },
    { name: "Hover", page: "hover", keys: ["hover"] },
    { name: "Signature Help", page: "signature-help", keys: ["signature_help"] },
    { name: "Code Navigation", page: "navigation", keys: ["navigation", "workspace_symbol"] },
    { name: "Document Links", page: "document-links", keys: ["document_links"] },
    { name: "Semantic Tokens", page: "semantic-tokens", keys: ["semantic_tokens"] },
    { name: "Inlay Hints", page: "inlay-hints", keys: ["inlay_hint"] },
    { name: "Folding Ranges", page: "folding-ranges", keys: ["folding_range"] },
    { name: "Document Symbols", page: "document-symbols", keys: ["document_symbol"] },
    { name: "Formatting", page: "formatting", label: "Implemented" },
    { name: "Diagnostics", page: "diagnostics", label: "Partial" },
    { name: "Code Action", page: "code-action", label: "Stub" },
];

const OVERVIEW_DOC = "docs/en/features/overview.md";
const OVERVIEW_MARKERS: RegionMarkers = {
    begin: /^<!-- BEGIN GENERATED OVERVIEW -->$/,
    end: "<!-- END GENERATED OVERVIEW -->",
};

const ISSUE_TRACKERS: Record<string, string> = {
    clangd: "https://github.com/clangd/clangd/issues/",
    vscode: "https://github.com/microsoft/vscode/issues/",
    llvm: "https://github.com/llvm/llvm-project/issues/",
};

const VALID_STATUS: readonly string[] = ["supported", "partial", "unsupported"];

const ITEM_MARKERS: RegionMarkers = {
    begin: /^<!-- BEGIN GENERATED ITEMS: (.+?) -->$/,
    end: "<!-- END GENERATED ITEMS -->",
};
const ISSUE_RE = /^([a-z]+)#(\d+)$/;

interface Fixture {
    path: string;
    section: string;
    name: string;
    status: string;
    issues: string[];
    summary: string;
    description: string;
    example: string;
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

/// Parse a fixture's doc header. Returns null for supplementary files.
function parseFixture(filePath: string, featureDir: string, problems: string[]): Fixture | null {
    const content = fs.readFileSync(filePath, "utf8");
    const relParts = path.relative(featureDir, filePath).split(path.sep);
    const isUnit = relParts[relParts.length - 1] === "main.cpp";
    const fixtureEntry = isUnit ? relParts.length <= 3 : relParts.length <= 2;
    if (!fixtureEntry) {
        return null;
    }
    const depth = relParts.length - (isUnit ? 1 : 0);
    const section = depth === 2 ? (relParts[0] ?? "") : "";
    const header = scanFixtureHeader(content);
    const [first, ...rest] = header.headings;
    if (first === undefined || headingLevel(first) !== 1) {
        // Not an h1 heading: supplementary fixture, not a doc item.
        return null;
    }
    const lines = header.lines;

    // `<section>/NN_name.cpp` or `<section>/NN_unit/main.cpp`: the section
    // directory keys the page region, the number orders the item.
    if (!section) {
        problems.push(
            `${filePath}: doc-item fixture must live in a section directory ` +
                "(<section>/NN_name.cpp or <section>/NN_unit/main.cpp)",
        );
    }
    const name = header.name;
    for (const heading of rest) {
        problems.push(
            `${filePath}: unexpected heading '${heading}' (a doc header has one '# title')`,
        );
    }
    if (!name) {
        problems.push(`${filePath}: empty name`);
    }

    for (const line of header.malformed) {
        problems.push(
            `${filePath}: malformed metadata line '${line}' ` +
                "(expected '- key: value'; separate the description with a bare ///)",
        );
    }
    const keys = new Map<string, string>();
    for (const { key, value } of header.meta) {
        if (!FIXTURE_META_KEYS.includes(key)) {
            problems.push(`${filePath}: unknown key '${key}'`);
        } else if (keys.has(key)) {
            problems.push(`${filePath}: duplicate ${key}`);
        }
        keys.set(key, value);
    }
    const bodyStart = header.bodyStart;

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

    // A plain `//` comment block opening with `// snap:` directly after the
    // header explains the fixture's snapshot mode to maintainers; it is not
    // part of the rendered example code. (A bare leading `//` comment stays:
    // e.g. the comment-folding example is itself a comment.)
    let exampleStart = bodyStart;
    while ((lines[exampleStart] ?? "").trim() === "" && exampleStart < lines.length) {
        exampleStart += 1;
    }
    if ((lines[exampleStart] ?? "").trim().startsWith("// snap:")) {
        while ((lines[exampleStart] ?? "").trim().startsWith("// snap:")) {
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
        name,
        status,
        issues,
        summary: header.summary,
        description: trimBlank(header.description).join("\n"),
        example,
    };
}

/// A section's region: one capability card per item. The card's markers
/// carry the status and issue references (verbatim for translation), the
/// paragraphs between them are the name, summary and description, and
/// the `snap` fence names the fixture; the site reads the fixture and its
/// snapshot from the synced test corpus and renders the example itself.
function renderSection(fixtures: Fixture[]): string {
    return fixtures.map(renderItem).join("\n\n");
}

function renderItem(fx: Fixture): string {
    const marker = [fx.status, ...fx.issues].join(" ");
    const out = [`<!-- BEGIN CAPABILITY: ${marker} -->`, "", `**${fx.name}**`];
    const paragraphs = [fx.summary, fx.description].filter((text) => text !== "");
    if (paragraphs.length > 0) {
        out.push("", paragraphs.join("\n\n"));
    }
    const rel = path.relative(REPO_ROOT, fx.path).split(path.sep).join("/");
    out.push("", "```snap", rel, "```", "", "<!-- END CAPABILITY -->");
    return out.join("\n");
}

function collectFixtures(feature: string, problems: string[]): Fixture[] {
    // Snapshot corpora migrated to tests/snap/ keep feeding the docs from
    // their new home; the rest still live under tests/data/.
    const snapDir = path.join(REPO_ROOT, "tests", "snap", feature);
    const dataDir = fs.existsSync(snapDir)
        ? snapDir
        : path.join(REPO_ROOT, "tests", "data", feature);
    return globCpp(dataDir).flatMap((filePath) => parseFixture(filePath, dataDir, problems) ?? []);
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

function rewriteDoc(
    docText: string,
    sections: Map<string, Fixture[]>,
    docPath: string,
    problems: string[],
): string {
    const { text, seen } = rewriteRegions(
        docText,
        docPath,
        ITEM_MARKERS,
        (section) => {
            const matched = sections.get(section) ?? [];
            if (matched.length === 0) {
                problems.push(`${docPath}: region '${section}' matches no fixtures`);
            }
            return renderSection(matched);
        },
        problems,
    );
    for (const section of sections.keys()) {
        if (!seen.has(section)) {
            problems.push(`${docPath}: section '${section}' has no matching marker region`);
        }
    }
    return text;
}

function processFeature(
    docRel: string,
    fixtures: Fixture[],
    problems: string[],
): [string, string, string] {
    const docPath = path.join(REPO_ROOT, docRel);

    // Duplicate names would render as indistinguishable cards. A section
    // spanning two corpora would interleave their independently-sorted
    // item order.
    const corpusOf = (fx: Fixture): string =>
        path.relative(REPO_ROOT, fx.path).split(path.sep)[2] ?? "";
    const nameOwner = new Map<string, string>();
    const sectionOwner = new Map<string, string>();
    for (const fx of fixtures) {
        const prev = nameOwner.get(fx.name);
        if (prev !== undefined) {
            problems.push(`${fx.path}: duplicate capability name '${fx.name}' (also in ${prev})`);
        } else {
            nameOwner.set(fx.name, fx.path);
        }
        const corpus = corpusOf(fx);
        const section = sectionOwner.get(fx.section);
        if (section !== undefined && section !== corpus) {
            problems.push(
                `${fx.path}: section '${fx.section}' spans corpora '${section}' and '${corpus}'`,
            );
        }
        sectionOwner.set(fx.section, corpus);
    }

    const sections = new Map<string, Fixture[]>();
    for (const fx of fixtures) {
        const list = sections.get(fx.section);
        if (list) {
            list.push(fx);
        } else {
            sections.set(fx.section, [fx]);
        }
    }

    // Windows runners check out docs as CRLF (only the test data is pinned
    // to LF); normalize so the $-anchored marker regexes match.
    const current = fs.readFileSync(docPath, "utf8").replaceAll("\r\n", "\n");
    const updated = rewriteDoc(current, sections, docPath, problems);
    return [docPath, current, updated];
}

/// Render the overview status matrix between its GENERATED OVERVIEW
/// markers: fixture-backed rows aggregate their corpus statuses, the rest
/// keep their hand-assigned labels.
function processOverview(
    fixturesByFeature: Map<string, Fixture[]>,
    problems: string[],
): [string, string, string] {
    const docPath = path.join(REPO_ROOT, OVERVIEW_DOC);
    const rows: string[][] = [["Feature", "Status", "Page"]];
    for (const row of OVERVIEW_ROWS) {
        let status = row.label ?? "";
        const fixtures = (row.keys ?? []).flatMap((key) => fixturesByFeature.get(key) ?? []);
        if (fixtures.length > 0) {
            const counts = new Map<string, number>();
            for (const fx of fixtures) {
                counts.set(fx.status, (counts.get(fx.status) ?? 0) + 1);
            }
            status = VALID_STATUS.filter((s) => counts.has(s))
                .map((s) => `${counts.get(s)} ${s}`)
                .join(" · ");
        }
        rows.push([row.name, status, `[${row.page}](./${row.page}.md)`]);
    }

    const current = fs.readFileSync(docPath, "utf8").replaceAll("\r\n", "\n");
    const { text: updated, seen } = rewriteRegions(
        current,
        docPath,
        OVERVIEW_MARKERS,
        () => renderMarkdownTable(rows).join("\n"),
        problems,
    );
    if (seen.size === 0) {
        problems.push(`${docPath}: missing GENERATED OVERVIEW region`);
    }
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
        console.error("usage: docs/feature.ts update|check");
        return 2;
    }

    const problems: string[] = [];
    for (const corpus of snapCorpora()) {
        for (const fixture of corpus.fixtures) {
            const entry = fixture.files.find((file) => file.rel === fixture.rel);
            if (entry !== undefined) {
                problems.push(
                    ...validateFixtureHeader(
                        entry.content,
                        path.join(corpus.corpus, entry.rel),
                        fixture.section,
                    ),
                );
            }
        }
    }
    const fixturesByFeature = new Map<string, Fixture[]>(
        Object.keys(FEATURES).map((feature) => [feature, collectFixtures(feature, problems)]),
    );
    // A doc page fed by several corpora is processed once with their
    // fixtures merged, so the later pass cannot clobber the earlier one.
    const docFeatures = new Map<string, string[]>();
    for (const [feature, docRel] of Object.entries(FEATURES)) {
        docFeatures.set(docRel, [...(docFeatures.get(docRel) ?? []), feature]);
    }
    const results = [...docFeatures.entries()].map(([docRel, features]) =>
        processFeature(
            docRel,
            features.flatMap((feature) => fixturesByFeature.get(feature) ?? []),
            problems,
        ),
    );
    results.push(processOverview(fixturesByFeature, problems));

    if (problems.length > 0) {
        console.error("feature docs: problems found:");
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
        console.error("feature docs: docs are stale; run 'docs/feature.ts update'");
        return 1;
    }
    return 0;
}

process.exit(main(process.argv.slice(2)));
