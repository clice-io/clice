/// An index too large for an IPC message reaches the master through a
/// transfer file, and is dropped with a warning when there is no cache store
/// to hold one. CLICE_TEST_MAX_INDEX_BYTES lowers the inline limit so every
/// index here takes that route.

import type { Serve } from "@clice/tools/actions";
import { at, expect, serve } from "../../fixtures.ts";

const HOOK = { CLICE_TEST_MAX_INDEX_BYTES: "1024" };
const FUNCTIONS = Array.from({ length: 200 }, (_, i) => `int function_${i}() { return ${i}; }`);
const BIG = `${FUNCTIONS.join("\n")}\nint use() { return function_0(); }\n`;

async function ownIndexWarnings(s: Serve, file: string): Promise<string[]> {
    return (await s.compiled(file))
        .map((d) => (typeof d.message === "string" ? d.message : d.message.value))
        .filter((m) => m.includes("the file's own index"));
}

async function transfersRemoved(s: Serve): Promise<void> {
    await s.sync();
    expect((await s.client.stats()).pendingTmpFiles, "transfer files left behind").toBe(0);
}

serve.files({ "big.cpp": BIG }, { env: HOOK, config: { project: { enable_indexing: false } } })(
    "open file index via file",
    async ({ s }) => {
        expect(await ownIndexWarnings(s, "big.cpp")).toEqual([]);
        expect(
            s
                .show(await s.references(at("big.cpp", "int |function_0()")))
                .split("\n")
                .sort(),
        ).toEqual([
            "big.cpp: int function_0() { return 0; }",
            "big.cpp: int use() { return function_0(); }",
        ]);
        await transfersRemoved(s);
    },
);

serve.files(
    {
        "h.h": "#pragma once\nint foo(int a);\n",
        "main.cpp": '#include "h.h"\nint main() { return foo(1); }\n',
        "b.cpp": `#include "h.h"\nint foo(int a) { return a; }\n${BIG}`,
    },
    { env: HOOK },
)("background index via file", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.indexed();
    expect(s.show(await s.definition(at("main.cpp", "foo(1)")))).toBe(
        "b.cpp: int foo(int a) { return a; }",
    );
    await transfersRemoved(s);
});

serve.files({ ".clice": "not a directory\n", "big.cpp": BIG }, { env: HOOK })(
    "oversized index without store",
    async ({ s }) => {
        const warnings = await ownIndexWarnings(s, "big.cpp");
        expect(warnings.length).toBe(1);
        expect(warnings[0]).toContain("too large to send between clice processes");
        expect(await s.hover(at("big.cpp", "int f|unction_0()"))).not.toBeNull();
    },
);
