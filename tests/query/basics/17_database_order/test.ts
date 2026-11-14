import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The answers list files and callers by place, whichever unit the database
// names first and the index reads first.
test(async (t) => {
    const questions = [
        "query file-deps shared.h --direction includers",
        "query impact-analysis shared.h",
        "query call-graph twice --direction callers",
    ];
    t.disk.database({ "zeta.cpp": [], "alpha.cpp": [] });
    await t.cli("index --jobs 1");
    const first: unknown[] = [];
    for (const question of questions) {
        first.push((await t.cli(question)).json);
    }
    t.disk.rm(".clice");
    t.disk.database({ "alpha.cpp": [], "zeta.cpp": [] });
    await t.cli("index --jobs 1");
    for (const [i, question] of questions.entries()) {
        expect((await t.cli(question)).json, question).toEqual(first[i]);
    }
});
