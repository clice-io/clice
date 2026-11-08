import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test(
    "header opened from a linked system directory",
    { except: { win32: "a symlink on Windows needs a privilege the CI runners lack" } },
    async (t) => {
        // The header is opened alone, by its own path; its includers reach
        // it as a system header through the link, and its typedef is one
        // symbol with theirs. Rendering resolves links: the spellings are
        // compared raw.
        t.disk.symlink("third/fmt", "cellar/fmt");
        const s = await t.start();
        expect(await s.errors("main.cpp")).toEqual([]);
        await s.indexed();
        expect(await s.errors("cellar/fmt/core.h")).toEqual([]);
        const name = t.at("cellar/fmt/core.h", "typedef int handle|_t;");
        const reply = await s.request<{ uri: string }[]>("textDocument/references", name, {
            context: { includeDeclaration: true },
        });
        expect(reply.map(({ uri }) => s.spelling(uri)).sort()).toEqual([
            "cellar/fmt/core.h",
            "cellar/fmt/core.h",
            "main.cpp",
        ]);
    },
);
