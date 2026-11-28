import * as fs from "node:fs";
import * as path from "node:path";
import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

/// A store's directories and files in `.clice/<dir>`, the store's version
/// and the hashes in directory and blob names elided.
function store(t: TestContext, dir: string): string[] {
    const cache = path.join(t.root, ".clice", dir);
    return fs
        .readdirSync(cache, { recursive: true, withFileTypes: true })
        .map((entry) => {
            const rel = path.relative(cache, path.join(entry.parentPath, entry.name));
            return rel.split(path.sep).join("/") + (entry.isDirectory() ? "/" : "");
        })
        .map((rel) =>
            rel
                .replace(/(^|\/)v\d+\//, "$1v#/")
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
    // The index in the cache directory, the PCHs and PCMs in the project's
    // directory of them, here the tests' `.clice/precompiled`.
    await t.show("store", store(t, "cache"));
    await t.show("precompiled", store(t, "precompiled"));
});
