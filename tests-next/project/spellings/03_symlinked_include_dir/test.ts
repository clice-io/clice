import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test(
    "header found through a symlinked directory",
    { except: { win32: "a symlink on Windows needs a privilege the CI runners lack" } },
    async (t) => {
        t.disk.symlink("inc", "vendor/real");
        const s = await t.start();
        expect(await s.errors("main.cpp")).toEqual([]);
        // Rendering resolves links: the spelling the server answers with is
        // compared raw. The header is named by its own path, not the one
        // the lookup went through.
        const call = t.at("main.cpp", "return get_|x(1)");
        const targets = await s.request<{ uri: string }[]>("textDocument/definition", call);
        expect(targets.map(({ uri }) => decodeURIComponent(uri))).toEqual([
            s.uri("vendor/real/utils.h"),
        ]);
        // The lookup's name is served too.
        expect(await s.errors("inc/utils.h")).toEqual([]);
        await t.show("hover", s.hover(t.at("inc/utils.h", "int get_|x")));
    },
);
