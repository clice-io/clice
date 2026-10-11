import * as fs from "node:fs";
import * as path from "node:path";
import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

/// The cache's directories and files, the store's version and the hashes
/// in blob names elided.
function store(t: TestContext): string[] {
    const cache = path.join(t.root, ".clice", "cache");
    return fs
        .readdirSync(cache, { recursive: true, withFileTypes: true })
        .map((entry) => {
            const rel = path.relative(cache, path.join(entry.parentPath, entry.name));
            return rel.split(path.sep).join("/") + (entry.isDirectory() ? "/" : "");
        })
        .map((rel) =>
            rel
                .replace(/^v\d+\//, "v#/")
                .replace(/[0-9a-f]{32}/g, "<key>")
                .replace(/[0-9a-f]{16}/g, "<nonce>"),
        )
        .sort();
}

test("store layout", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.indexed();
    // The store is final once the server is gone: no write in flight.
    await s.stop();
    await t.show("store", store(t));
});
