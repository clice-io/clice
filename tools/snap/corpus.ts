/// Corpus model of the snap domain — the single source of truth for what a
/// fixture is: enumeration and unit detection, the strict frontmatter
/// schema, the corpus.json manifest, and materialization of one fixture
/// into a throwaway workspace for the server driver.
///
/// A fixture is either a single `.cpp` at the corpus root or a
/// subdirectory entered through its `main.cpp` — one multi-file unit whose
/// sibling sources (module interfaces, headers, extra sources) belong to
/// the fixture. Everything else in the corpus is support material shared
/// by all fixtures (include roots, `.clang-format`, ...).

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

const META_KEYS = [
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

export function parseFixtureMeta(content: string, filePath: string): FixtureMeta {
    const meta: FixtureMeta = {
        status: "supported",
        verify: "both",
        snap: "shared",
        diagnostics: false,
        indexing: false,
        flags: [],
    };

    // Scan only the metadata list of the leading `///` block: heading
    // lines and blank separators before it, then `- key: value` lines
    // until the next blank `///`. The markdown description after that may
    // legitimately contain bulleted `word:` lines (feature_docs renders
    // it) — mirroring parseFixture in tools/feature_docs.ts, they are not
    // metadata. A doc heading is not required: a supplementary fixture
    // (no `# ` title, ignored by feature_docs) may still open with a bare
    // `///` meta block.
    let inMeta = false;
    const seen = new Set<string>();
    for (let line of content.split("\n")) {
        line = line.trim();
        if (!line.startsWith("///")) {
            break;
        }
        line = line.slice(3).trim();
        if (line.startsWith("#")) {
            continue;
        }
        if (line === "") {
            if (inMeta) {
                break;
            }
            continue;
        }
        // Any `- something:` at the metadata position is treated as a key so
        // that a misspelling (`- Snap:`, `- snap :`) errors instead of
        // silently ending the block on defaults.
        const match = /^- ([^:]+):(.*)$/.exec(line);
        if (!match) {
            break;
        }
        inMeta = true;
        const key = (match[1] ?? "").trim();
        const value = (match[2] ?? "").trim();
        if (!META_KEYS.includes(key)) {
            throw new Error(`${filePath}: unknown fixture meta key '${key}'`);
        }
        // A repeated key (merge leftovers, copy/paste) must not silently
        // let the later value win — it could flip a snap mode unnoticed.
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

const C_FAMILY = /\.(cpp|cc|cxx|c|cppm|h|hpp|hh)$/;
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

/// Enumerate the corpora under tests/snap.
export function snapCorpora(): SnapCorpus[] {
    const corpora: SnapCorpus[] = [];
    for (const feature of fs.readdirSync(SNAP_DIR).sort()) {
        const corpus = path.join(SNAP_DIR, feature);
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
        }
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
            if (rel.endsWith(".cpp") && !rel.includes("/")) {
                fixtures.push(makeFixture(corpus, feature, rel, "", [rel], []));
                continue;
            }
            if (rel.endsWith(".cpp")) {
                throw new Error(
                    `tests/snap/${feature}/${rel}: fixture sources live at the corpus ` +
                        "root or in a main.cpp unit",
                );
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
            fixtures.push(makeFixture(corpus, feature, `${unit}/main.cpp`, unit, sources, extras));
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
    return { rel, unit, meta, files, extras, active };
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
    // Unit sources compile with the unit directory as cwd, mirroring
    // unit_directory on the inspect path, so relative compiler operands
    // (-Iinclude, @args.rsp, ...) resolve identically on both paths.
    // Support sources are never inspected; their cwd is where they live.
    const unitDir = fixture.unit === "" ? posixRoot : `${posixRoot}/${fixture.unit}`;
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
