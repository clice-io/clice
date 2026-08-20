/// Tests for the snapshot format, layouts and the URI validator
/// (tools/snap/snapshot.ts).

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { expect, test } from "vitest";
import { URI } from "vscode-uri";
import {
    formatSnap,
    normalizeFileUri,
    parseSnap,
    SnapshotContext,
    yamlStr,
} from "@clice/tools/snap/snapshot";

test("snap format round trip", () => {
    const text = formatSnap("f.cpp", "body\n", "2026-01-01");
    expect(parseSnap(text)).toEqual({ createdAt: "2026-01-01", body: "body\n" });
    expect(parseSnap("no frontmatter")).toBeNull();
});

test("snapshot check flows", () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "snap-"));
    const snapPath = path.join(dir, "a.cpp.snap.yml");

    const ctx = new SnapshotContext(dir);
    ctx.check("a.cpp", "one\n"); // first run creates
    const createdAt = parseSnap(fs.readFileSync(snapPath, "utf8"))!.createdAt;
    ctx.check("a.cpp", "one\n"); // match passes

    expect(() => {
        ctx.check("a.cpp", "two\n");
    }).toThrow("snapshot mismatch");
    expect(fs.existsSync(`${snapPath}.new`)).toBe(true);

    // A non-owning context (update: false) compares but never authors:
    // a missing snapshot is the owner's to create.
    const readonly = new SnapshotContext(dir, { update: false });
    readonly.check("a.cpp", "one\n");
    expect(() => {
        readonly.check("b.cpp", "x\n");
    }).toThrow("missing snapshot");

    new SnapshotContext(dir, { update: true }).check("a.cpp", "two\n");
    expect(parseSnap(fs.readFileSync(snapPath, "utf8"))).toEqual({
        createdAt,
        body: "two\n",
    });
    expect(fs.existsSync(`${snapPath}.new`)).toBe(false);
    ctx.check("a.cpp", "two\n");
});

test("colocated snapshot layout", () => {
    const ctx = new SnapshotContext("/corpus", { colocated: true });
    expect(ctx.snapPath("group/a.cpp")).toBe(path.join("/corpus", "group/a.snap.yml"));
    expect(ctx.snapPath("group/a.cpp", "server")).toBe(
        path.join("/corpus", "group/a.server.snap.yml"),
    );
    expect(new SnapshotContext("/dir").snapPath("a.cpp")).toBe(path.join("/dir", "a.cpp.snap.yml"));
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

test("yaml string escapes", () => {
    expect(yamlStr('a"b\\c\n\t\x01')).toBe('"a\\"b\\\\c\\n\\t\\x01"');
});
