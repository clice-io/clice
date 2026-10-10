/// An index too large for an IPC message reaches the master through a
/// transfer file, and is dropped with a warning when there is no cache store
/// to hold one. CLICE_TEST_MAX_INDEX_BYTES lowers the inline limit so every
/// index here takes that route.

import type { Serve } from "@clice/tools/actions";
import { at, expect, serve } from "../../fixtures.ts";

const HOOK = { CLICE_TEST_MAX_INDEX_BYTES: "1" };

async function ownIndexWarnings(s: Serve, file: string): Promise<string[]> {
    return (await s.compiled(file))
        .map((d) => (typeof d.message === "string" ? d.message : d.message.value))
        .filter((m) => m.includes("the file's own index"));
}

async function transfersRemoved(s: Serve): Promise<void> {
    await s.sync();
    expect((await s.stats()).pendingTmpFiles, "transfer files left behind").toBe(0);
}

serve("tiny", { env: HOOK, config: { project: { enable_indexing: false } } })(
    "open file index via file",
    async ({ s }) => {
        expect(await ownIndexWarnings(s, "main.cpp")).toEqual([]);
        expect(
            s
                .show(await s.references(at("main.cpp", "int |add(")))
                .split("\n")
                .sort(),
        ).toEqual(["main.cpp: int add(int lhs, int rhs) {", "main.cpp: int value = add(1, 2);"]);
        await transfersRemoved(s);
    },
);

serve("shapes/headers", { env: HOOK })("background index via file", async ({ s }) => {
    await s.compiled(s.file("main"));
    await s.indexed();
    expect(s.show(await s.definition(at(s.file("main"), "registry_co|unt()")))).toBe(
        "src/registry.cpp: int registry_count() {",
    );
    await transfersRemoved(s);
});

serve("tiny", { files: { ".clice": "not a directory\n" }, env: HOOK })(
    "oversized index without store",
    async ({ s }) => {
        const warnings = await ownIndexWarnings(s, "main.cpp");
        expect(warnings.length).toBe(1);
        expect(warnings[0]).toContain("too large to send between clice processes");
        expect(await s.hover(at("main.cpp", "int a|dd("))).not.toBeNull();
    },
);
