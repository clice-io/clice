/// Tests for the corpus model (tools/snap/corpus.ts): the strict fixture
/// frontmatter schema.

import { expect, test } from "vitest";
import { parseFixtureMeta } from "@clice/tools/snap/corpus";

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
    // bare `///` meta block.
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
