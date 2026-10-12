import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A worker killed from outside while it compiles several documents names
// none of them: their work is sent again and none is blamed. When the
// resent work dies too, the requests waiting on it answer empty, and the
// next request compiles again.

const FILES = ["a.cpp", "b.cpp", "c.cpp"];

test(async (t) => {
    const s = await t.start({
        config: { diagnostics: { clang_tidy: false }, project: { enable_indexing: false } },
        killWorkers: true,
    });
    const rounds = [];
    for (let round = 0; round < 2; round++) {
        rounds.push(
            await Promise.all(FILES.map((file) => s.park("compile", file, { at: "worker" }))),
        );
    }
    for (const file of FILES) {
        s.open(file);
    }
    const hovers = Object.fromEntries(FILES.map((file) => [file, s.hover(t.at(file, "int |add"))]));
    for (const parks of rounds) {
        await Promise.all(parks.map((park) => park.reached()));
        expect(await s.killWorkers("stateful")).toBe(1);
    }
    await t.show("hovers", hovers);
    for (const file of FILES) {
        await t.show(`${file} pushes`, s.pushes(file));
    }
    await t.show(
        "hovers asked again",
        Object.fromEntries(FILES.map((file) => [file, s.hover(t.at(file, "int |add"))])),
    );
    for (const file of FILES) {
        await t.show(`${file} pushes after`, s.pushes(file));
    }
});
