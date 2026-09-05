/// Tests for the corpus model (tools/snap/corpus.ts): the strict fixture
/// frontmatter schema and snapshot ownership.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { expect, test } from "vitest";
import { parseAnnotations } from "@clice/tools/snap/annotation";
import {
    type SnapCorpus,
    type SnapFixture,
    fixtureRelative,
    headingLevel,
    orphanSnapshots,
    parseFixtureMeta,
    scanFixtureHeader,
    snapCorpora,
    validateFixtureHeader,
} from "@clice/tools/snap/corpus";

const DEFAULTS = {
    status: "supported",
    verify: "both",
    snap: "shared",
    diagnostics: false,
    indexing: false,
    flags: [],
};

test("fixture meta parsing", () => {
    const header = "/// # Title\n///\n/// - status: partial\n/// - snap: separate\nint x;\n";
    expect(parseFixtureMeta(header, "f")).toEqual({
        ...DEFAULTS,
        status: "partial",
        snap: "separate",
    });
    expect(parseFixtureMeta("/// # T\n///\n/// - snap: skip\n", "f").snap).toBe("skip");
    // Bulleted lines in the markdown description after the blank `///`
    // separator are prose, not metadata.
    const withDesc =
        "/// # T\n///\n/// ## I\n///\n/// - status: partial\n///\n/// - lorem: prose\nint x;\n";
    expect(parseFixtureMeta(withDesc, "f")).toEqual({ ...DEFAULTS, status: "partial" });
    // No header: defaults — verify both, one shared snapshot.
    expect(parseFixtureMeta("int x;\n", "f")).toEqual(DEFAULTS);
    // A supplementary fixture (no `# ` doc title) may still open with a
    // plain-comment meta block.
    expect(parseFixtureMeta("// - diagnostics: expected\n\nint x;\n", "f").diagnostics).toBe(true);
    // The legacy `///` spelling remains readable so validation can report
    // it as an R7 migration error.
    expect(parseFixtureMeta("/// - diagnostics: expected\n\nint x;\n", "f").diagnostics).toBe(true);
    expect(() => parseFixtureMeta("/// # T\n///\n/// - snpa: separate\n", "f")).toThrow(
        "unknown fixture meta key",
    );
    // Near-miss spellings must error too, not silently end the block.
    expect(() => parseFixtureMeta("/// # T\n///\n/// - Snap: shared\n", "f")).toThrow(
        "unknown fixture meta key",
    );
    expect(() => parseFixtureMeta("/// # T\n///\n/// - snap: shred\n", "f")).toThrow(
        "invalid snap mode",
    );
    // A repeated key must not silently let the later value win.
    expect(() =>
        parseFixtureMeta("/// # T\n///\n/// - snap: shared\n/// - snap: skip\n", "f"),
    ).toThrow("duplicate fixture meta key");
});

test("verify and snap axes", () => {
    expect(parseFixtureMeta("/// # T\n///\n/// - verify: server\n", "f").verify).toBe("server");
    expect(parseFixtureMeta("/// # T\n///\n/// - verify: inspect\n", "f").verify).toBe("inspect");
    expect(() => parseFixtureMeta("/// # T\n///\n/// - verify: wire\n", "f")).toThrow(
        "invalid verify mode",
    );
    // The snap axis relates the two paths of a `verify: both` fixture; on
    // a single-path fixture it can only be a mistake.
    expect(() =>
        parseFixtureMeta("/// # T\n///\n/// - verify: server\n/// - snap: separate\n", "f"),
    ).toThrow("requires verify: both");
    expect(() =>
        parseFixtureMeta("/// # T\n///\n/// - snap: skip\n/// - verify: inspect\n", "f"),
    ).toThrow("requires verify: both");
});

test("diagnostics, indexing and flags keys", () => {
    const meta = parseFixtureMeta(
        "/// # T\n///\n" +
            "/// - diagnostics: expected\n" +
            "/// - indexing: true\n" +
            '/// - flags: ["-std=c++26"]\n',
        "f",
    );
    expect(meta.diagnostics).toBe(true);
    expect(meta.indexing).toBe(true);
    expect(meta.flags).toEqual(["-std=c++26"]);
    expect(() => parseFixtureMeta("/// # T\n///\n/// - diagnostics: yes\n", "f")).toThrow(
        "invalid diagnostics value",
    );
    expect(() => parseFixtureMeta("/// # T\n///\n/// - indexing: on\n", "f")).toThrow(
        "invalid indexing value",
    );
    expect(() => parseFixtureMeta("/// # T\n///\n/// - flags: -std=c++26\n", "f")).toThrow(
        "not a JSON array",
    );
    expect(() => parseFixtureMeta("/// # T\n///\n/// - flags: [1]\n", "f")).toThrow(
        "JSON string array",
    );
});

test("snapshot ownership follows verify and snap modes", () => {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clice-corpus-"));
    try {
        for (const name of [
            "shared.snap.yml",
            "split.inspect.snap.yml",
            "split.server.snap.yml",
            "split.snap.yml", // superseded by the separate variants
            "skipped.snap.yml", // a skip fixture keeps no snapshot
            "renamed.snap.yml", // no fixture at all
        ]) {
            fs.writeFileSync(path.join(tmp, name), "---\n---\n");
        }
        const fixture = (rel: string, meta: Partial<SnapFixture["meta"]>): SnapFixture => ({
            rel,
            unit: "",
            section: "",
            meta: {
                status: "supported",
                verify: "both",
                snap: "shared",
                diagnostics: false,
                indexing: false,
                flags: [],
                ...meta,
            },
            files: [],
            extras: [],
            active: meta.snap !== "skip",
        });
        const corpus: SnapCorpus = {
            feature: "demo",
            corpus: tmp,
            flags: [],
            configSection: "demo",
            fixtures: [
                fixture("shared.cpp", {}),
                fixture("split.cpp", { snap: "separate" }),
                fixture("skipped.cpp", { snap: "skip" }),
            ],
            support: [],
        };
        expect(orphanSnapshots(corpus).sort()).toEqual([
            "renamed.snap.yml",
            "skipped.snap.yml",
            "split.snap.yml",
        ]);
    } finally {
        fs.rmSync(tmp, { recursive: true, force: true });
    }
});

test("fixture header scanning", () => {
    const content = [
        "/// # Qualified name",
        "///",
        "/// - status: partial",
        "/// - verify: server",
        "///",
        "/// The card summarizes the capability",
        "///",
        "/// Further prose, with",
        "///   - a bullet",
        "",
        "// snap: The server path supplies the required index.",
        "int x;",
        "",
    ].join("\n");
    const header = scanFixtureHeader(content);
    expect(header.headings).toEqual(["# Qualified name"]);
    expect(header.name).toBe("Qualified name");
    expect(header.meta).toEqual([
        { key: "status", value: "partial" },
        { key: "verify", value: "server" },
    ]);
    expect(header.summary).toBe("The card summarizes the capability");
    expect(header.description).toEqual(["", "Further prose, with", "  - a bullet"]);
    expect(header.notes).toEqual(["The server path supplies the required index."]);
    expect(header.lines[header.bodyStart]).toBe("");
    expect(header.lines[header.bodyStart + 2]).toBe("int x;");
    // No header at all.
    const bare = scanFixtureHeader("int x;\n");
    expect(bare.headings).toEqual([]);
    expect(bare.bodyStart).toBe(0);
    // A leading `///` block that opens with prose is a doc comment on the
    // code, not a header: nothing is malformed and the snap suite sees the
    // defaults.
    const doc = scanFixtureHeader("/// Documents f.\n/// - not: a key\nint f();\n");
    expect(doc).toMatchObject({ headings: [], meta: [], malformed: [], bodyStart: 0 });
    expect(parseFixtureMeta("/// Documents f.\nint f();\n", "f")).toEqual(DEFAULTS);
    // So are a bullet without a colon and a `#` line that is no heading.
    for (const prose of ["/// - first bullet\nint f();\n", "/// #include usage\nint f();\n"]) {
        expect(scanFixtureHeader(prose)).toMatchObject({
            headings: [],
            malformed: [],
            bodyStart: 0,
        });
        expect(parseFixtureMeta(prose, "f")).toEqual(DEFAULTS);
    }
    // A colon-bearing bullet is an entry attempt even when misspelled.
    expect(() => parseFixtureMeta("/// - snap : skip\n", "f")).toThrow(
        "malformed fixture meta line",
    );
    // Headings are markdown headings of any level; `##x` is not one.
    const levels = scanFixtureHeader("/// # T\n/// ### Sub\n/// ##x\n");
    expect(levels.headings).toEqual(["# T", "### Sub"]);
    expect(levels.malformed).toEqual(["##x"]);
    expect(["# T", "##\tT", "###", "##x", "- x: y"].map(headingLevel)).toEqual([1, 2, 3, 0, 0]);
    // Without a list, the blank `///` after the headings separates the
    // description (bullets in it are prose); unseparated prose is a
    // malformed entry.
    const prose = scanFixtureHeader("/// # T\n///\n/// Documents T.\n/// - a: bullet\nint x;\n");
    expect(prose).toMatchObject({ headings: ["# T"], meta: [], malformed: [] });
    expect(prose.summary).toBe("Documents T.\n- a: bullet");
    expect(prose.description).toEqual([]);
    expect(prose.lines[prose.bodyStart]).toBe("int x;");
    expect(parseFixtureMeta("/// # T\n///\n/// Documents T.\n", "f")).toEqual(DEFAULTS);
    expect(scanFixtureHeader("/// # T\n/// Documents T.\n").malformed).toEqual(["Documents T."]);
    // Blank `///` lines before the opening heading are skipped.
    expect(scanFixtureHeader("///\n/// # T\n///\n/// - snap: skip\n").headings).toEqual(["# T"]);
    expect(parseFixtureMeta("///\n/// - snap: skip\n", "f").snap).toBe("skip");
});

test("fixture header validation", () => {
    const valid = [
        "/// # Qualified name",
        "///",
        "/// - status: supported",
        "/// - issues: clangd#710",
        "/// - verify: server",
        "///",
        "/// The hover card includes its enclosing scope",
        "///",
        "/// Further reader-facing detail.",
        "",
        "// snap: The server path supplies the required index.",
        "int x;",
        "",
    ].join("\n");
    expect(validateFixtureHeader(valid, "fixture.cpp", "hover")).toEqual([]);

    const invalid = [
        "// attribution",
        "",
        "/// # An excessively long fixture title — with details.",
        "///",
        "/// - verify: both",
        "/// - status: supported",
        "///",
        "/// this fixture pins a snapshot. It says two sentences",
        "",
        "int x;",
        "// snap: misplaced",
        "",
    ].join("\n");
    const problems = validateFixtureHeader(invalid, "fixture.cpp", "hover");
    for (const rule of ["R1", "R2", "R3", "R4", "R5", "R6"]) {
        expect(problems.some((item) => item.includes(`${rule}:`))).toBe(true);
    }
    expect(problems.every((item) => /^fixture\.cpp:\d+: R\d:/.test(item))).toBe(true);

    expect(validateFixtureHeader("int x;\n", "root.cpp", "")).toEqual([]);
    expect(
        validateFixtureHeader("/// - verify: server\nint x;\n", "nested.cpp", "section"),
    ).toEqual(expect.arrayContaining([expect.stringContaining("R7:")]));
});

test("fixture files are named relative to the fixture", () => {
    const single: SnapFixture = {
        rel: "fold_kinds/01_block.cpp",
        unit: "",
        section: "fold_kinds",
        meta: { ...DEFAULTS, flags: [] } as SnapFixture["meta"],
        files: [],
        extras: [],
        active: true,
    };
    const file = (rel: string) => ({ rel, content: "", source: parseAnnotations("") });
    expect(fixtureRelative(single, file("fold_kinds/01_block.cpp"))).toBe("01_block.cpp");
    const unit: SnapFixture = {
        ...single,
        rel: "modules/03_iface/main.cpp",
        unit: "modules/03_iface",
    };
    expect(fixtureRelative(unit, file("modules/03_iface/main.cpp"))).toBe("main.cpp");
    expect(fixtureRelative(unit, file("modules/03_iface/widget.cppm"))).toBe("widget.cppm");
});

test("documented fixtures live numbered in section directories", () => {
    // The corpus layout is what places an item on its feature page: a
    // fixture with a doc header outside a section directory, or without
    // the ordering prefix, would render nowhere or in an unstable order.
    for (const corpus of snapCorpora()) {
        for (const fixture of corpus.fixtures) {
            const entry = fixture.files.find((file) => file.rel === fixture.rel);
            const header = scanFixtureHeader(entry?.content ?? "");
            const documented =
                header.headings[0] !== undefined && headingLevel(header.headings[0]) === 1;
            if (!documented) {
                continue;
            }
            const name =
                fixture.unit === ""
                    ? fixture.rel.split("/").at(-1)
                    : fixture.unit.split("/").at(-1);
            expect(fixture.section, `${corpus.feature}/${fixture.rel}`).not.toBe("");
            expect(name, `${corpus.feature}/${fixture.rel}`).toMatch(/^\d\d_/);
        }
    }
});

test("corpus layout: sections, units and the depth limits", () => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "clice-corpus-"));
    const write = (rel: string, content = "int x;\n") => {
        fs.mkdirSync(path.dirname(path.join(root, rel)), { recursive: true });
        fs.writeFileSync(path.join(root, rel), content);
    };
    try {
        write("f/edge.cpp");
        write("f/sec/01_item.cpp");
        write("f/sec/02_unit/main.cpp");
        write("f/sec/02_unit/part.cppm");
        write("f/root_unit/main.cpp");
        write("f/inc/shared.h");
        write("f/sec/local.h");
        const [corpus] = snapCorpora(root);
        expect(corpus).toBeDefined();
        const fixtures = corpus!.fixtures.map((fx) => [fx.rel, fx.unit, fx.section]);
        expect(fixtures).toEqual([
            ["edge.cpp", "", ""],
            ["root_unit/main.cpp", "root_unit", ""],
            ["sec/01_item.cpp", "", "sec"],
            ["sec/02_unit/main.cpp", "sec/02_unit", "sec"],
        ]);
        // Support material lives at any depth; unit files belong to their unit.
        expect(corpus!.support).toEqual(["inc/shared.h", "sec/local.h"]);
        expect(corpus!.fixtures[3]!.files.map((file) => file.rel)).toEqual([
            "sec/02_unit/main.cpp",
            "sec/02_unit/part.cppm",
        ]);

        write("f/sec/deeper/x.cpp");
        expect(() => snapCorpora(root)).toThrow("fixture sources live at the corpus root");
        fs.rmSync(path.join(root, "f/sec/deeper"), { recursive: true });
        write("f/sec/deeper/unit/main.cpp");
        expect(() => snapCorpora(root)).toThrow("a unit lives at the corpus root");
        fs.rmSync(path.join(root, "f/sec/deeper"), { recursive: true });
        write("f/root_unit/inner/main.cpp");
        expect(() => snapCorpora(root)).toThrow("nested fixture units");
    } finally {
        fs.rmSync(root, { recursive: true, force: true });
    }
});
