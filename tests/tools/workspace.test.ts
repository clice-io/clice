/// Tests for workspace helpers (tools/client/workspace.ts).

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { expect, test } from "vitest";
import { Workspace, canonicalUri } from "@clice/tools/workspace";

test("canonical uri edges", () => {
    // The identity canonicalizer must be a faithful percent-decode:
    // collision-free for literal '%', inert on '+', UTF-8 aware, and an
    // identity on malformed input.
    expect(canonicalUri("file:///ws/a%20b.h")).toBe("file:///ws/a b.h");
    expect(canonicalUri("file:///ws/a%2520b.h")).toBe("file:///ws/a%20b.h");
    expect(canonicalUri("file:///ws/a%2520b.h")).not.toBe(canonicalUri("file:///ws/a%20b.h"));
    expect(canonicalUri("file:///ws/a+b.h")).toBe("file:///ws/a+b.h");
    expect(canonicalUri("file:///ws/%E6%97%A5.h")).toBe("file:///ws/日.h");
    expect(canonicalUri("file:///ws/%GG.h")).toBe("file:///ws/%GG.h");
    // The two drive-colon spellings — client-encoded and server-literal —
    // collapse to one identity.
    expect(canonicalUri("file:///c%3A/x.h")).toBe(canonicalUri("file:///c:/x.h"));
});

test("cache root follows the store's version directory", () => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "clice-ws-"));
    try {
        const ws = new Workspace(root);
        // No store yet: the blob helpers see empty namespaces, while a
        // caller that needs the root itself is told there is none.
        expect(ws.pchFiles()).toEqual([]);
        expect(ws.tmpFiles()).toEqual([]);
        expect(() => ws.cacheRoot()).toThrow(/found \[\]/);

        fs.mkdirSync(path.join(root, ".clice", "cache", "v9", "pch"), { recursive: true });
        fs.writeFileSync(path.join(root, ".clice", "cache", "v9", "pch", "k.pch"), "");
        expect(ws.cacheRoot()).toBe(path.join(root, ".clice", "cache", "v9"));
        expect(ws.pchFiles()).toEqual([path.join(root, ".clice", "cache", "v9", "pch", "k.pch")]);

        // Two versions side by side would let a test read the wrong one.
        fs.mkdirSync(path.join(root, ".clice", "cache", "v10"));
        expect(() => ws.cacheRoot()).toThrow(/found \[v10, v9\]|found \[v9, v10\]/);
    } finally {
        fs.rmSync(root, { recursive: true, force: true });
    }
});
