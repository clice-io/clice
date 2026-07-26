/// Tests for the snapshot tooling itself: annotation parser, snapshot
/// format and flows, URI validator. No server involved.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { expect, test } from "vitest";
import { URI } from "vscode-uri";
import { parseAnnotations } from "@clice/tools/annotation";
import { decodeSemanticTokens } from "@clice/tools/presenters";
import {
    fixtureFrontmatter,
    formatSnap,
    normalizeFileUri,
    parseSnap,
    SnapshotContext,
    yamlStr,
} from "@clice/tools/snapshot";
import { canonicalUri } from "@clice/tools/workspace";

test("annotation passthrough", () => {
    const src = parseAnnotations("int x = 1;\n");
    expect(src.content).toBe("int x = 1;\n");
    expect(src.offsets.size).toBe(0);
    expect(src.ranges.size).toBe(0);
    expect(src.namelessOffsets).toEqual([]);
});

test("annotation points and ranges", () => {
    const src = parseAnnotations("int §(a)x = §1;\n§(r)⟦int §⟦y⟧;⟧\n");
    expect(src.content).toBe("int x = 1;\nint y;\n");
    expect(src.offsets).toEqual(new Map([["a", 4]]));
    expect(src.namelessOffsets).toEqual([8]);
    expect(src.ranges).toEqual(
        new Map([
            ["r", [11, 17]],
            ["", [15, 16]],
        ]),
    );
});

test("annotation byte offsets", () => {
    // Offsets count UTF-8 bytes, matching the C++ side.
    const src = parseAnnotations("/*中*/§(p)x");
    expect(src.content).toBe("/*中*/x");
    expect(src.offsets).toEqual(new Map([["p", 7]]));
});

test("annotation nameless parens", () => {
    const src = parseAnnotations("f§()(1)");
    expect(src.content).toBe("f(1)");
    expect(src.namelessOffsets).toEqual([1]);
});

test.for([
    "§(unterminated",
    "§(not an identifier)",
    "§(café)",
    "§(dup)x §(dup)y",
    "no open⟧",
    "§(d)⟦x⟧ §(d)⟦y⟧",
    "bare ⟦",
    "§⟦unclosed",
])("annotation rejects %s", (text) => {
    expect(() => parseAnnotations(text)).toThrow();
});

test("snap format round trip", () => {
    const text = formatSnap("s.ts", "f.cpp", "body\n", "2026-01-01");
    expect(parseSnap(text)).toEqual({ createdAt: "2026-01-01", body: "body\n" });
    expect(parseSnap("no frontmatter")).toBeNull();
});

test("snapshot check flows", () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "snap-"));
    const snapPath = path.join(dir, "a.cpp.snap.yml");

    const ctx = new SnapshotContext(dir, false);
    ctx.check("a.cpp", "one\n"); // first run creates
    const createdAt = parseSnap(fs.readFileSync(snapPath, "utf8"))!.createdAt;
    ctx.check("a.cpp", "one\n"); // match passes

    expect(() => {
        ctx.check("a.cpp", "two\n");
    }).toThrow("snapshot mismatch");
    expect(fs.existsSync(`${snapPath}.new`)).toBe(true);

    new SnapshotContext(dir, true).check("a.cpp", "two\n");
    expect(parseSnap(fs.readFileSync(snapPath, "utf8"))).toEqual({
        createdAt,
        body: "two\n",
    });
    expect(fs.existsSync(`${snapPath}.new`)).toBe(false);
    ctx.check("a.cpp", "two\n");
});

test("normalize file uri", () => {
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uri-"));
    const ws = path.join(tmp, "ws");
    fs.mkdirSync(ws);
    const inside = path.join(ws, "a b.h");
    fs.writeFileSync(inside, "");
    const outside = path.join(tmp, "outside.h");
    fs.writeFileSync(outside, "");

    const insideUri = URI.file(inside).toString();
    expect(insideUri).toContain("%20"); // the positive case exercises decoding
    expect(normalizeFileUri(insideUri, ws)).toBe("${WS}/a b.h");

    const plain = path.join(ws, "plain.h");
    fs.writeFileSync(plain, "");

    for (const bad of [
        inside, // raw path, no scheme
        plain, // raw path without spaces; URI.parse would silently upgrade it
        insideUri.replaceAll("%20", " "), // missing percent-encoding
        "file:///tmp/%GG.h", // malformed percent triplet
        "https://example.com/a.h", // wrong scheme
        `file://host${inside.split(path.sep).join("/")}`, // unexpected authority
        `${insideUri}?query`,
        `${insideUri}#fragment`,
        "file:relative.h", // not absolute
        URI.file(path.join(ws, "missing.h")).toString(), // target does not exist
        URI.file(outside).toString(), // escapes the workspace
    ]) {
        expect(() => normalizeFileUri(bad, ws), bad).toThrow();
    }
});

test("canonical uri edges", () => {
    // The identity canonicalizer must be a faithful percent-decode:
    // collision-free for literal '%', inert on '+', UTF-8 aware, and an
    // identity on malformed input.
    expect(canonicalUri("file:///ws/a%20b.h")).toBe("file:///ws/a b.h");
    expect(canonicalUri("file:///ws/a%2520b.h")).toBe("file:///ws/a%20b.h");
    expect(canonicalUri("file:///ws/a%2520b.h")).not.toBe(canonicalUri("file:///ws/a%20b.h"));
    expect(canonicalUri("file:///ws/a+b.h")).toBe("file:///ws/a+b.h");
    expect(canonicalUri("file:///ws/%E6%97%A5.h")).toBe("file:///ws/\u65e5.h");
    expect(canonicalUri("file:///ws/%GG.h")).toBe("file:///ws/%GG.h");
    // The two drive-colon spellings — client-encoded and server-literal —
    // collapse to one identity.
    expect(canonicalUri("file:///c%3A/x.h")).toBe(canonicalUri("file:///c:/x.h"));
});

test("yaml string escapes", () => {
    expect(yamlStr('a"b\\c\n\t\x01')).toBe('"a\\"b\\\\c\\n\\t\\x01"');
});

test("fixture frontmatter", () => {
    const header = "/// # Title\n///\n/// - status: unsupported\nint x;\n";
    expect(fixtureFrontmatter(header, "status")).toBe("unsupported");
    expect(fixtureFrontmatter(header, "missing")).toBe("");
    expect(fixtureFrontmatter("int x;\n", "status")).toBe("");
});

test("semantic token decoding", () => {
    const legend = {
        tokenTypes: ["Type", "Function"],
        tokenModifiers: ["Definition", "Readonly"],
    };
    const data = [0, 4, 3, 0, 1, 1, 2, 4, 1, 3];
    const lines = ["abc defg", "xxfuncy"];
    expect(decodeSemanticTokens(data, lines, legend)).toEqual([
        '- { loc: "0:4", text: "def", kind: Type, modifiers: [Definition] }',
        '- { loc: "1:2", text: "func", kind: Function, modifiers: [Definition, Readonly] }',
    ]);
});
