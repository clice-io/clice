import { expect } from "vitest";
import { test, type Serve, type TestContext } from "@clice/tools/testing";

/// The files a request's locations name, as the server spells them.
async function files(
    s: Serve,
    method: string,
    at: ReturnType<TestContext["at"]>,
    extra: object = {},
): Promise<string[]> {
    const reply = await s.request<{ uri: string }[]>(method, at, extra);
    return reply.map(({ uri }) => s.spelling(uri)).sort();
}

test(
    "shared sources linked into the folder",
    { except: { win32: "a symlink on Windows needs a privilege the CI runners lack" } },
    async (t) => {
        // The folder links in sources kept outside it; the database names
        // them through the link, and so do the results, rather than leave
        // the folder for the link's target. Rendering resolves links: the
        // spellings are compared raw.
        t.disk.symlink("ws/shared", "source");
        const s = await t.start({ folders: ["ws"] });
        expect(await s.errors("ws/main.c")).toEqual([]);
        await s.indexed();
        const call = t.at("ws/main.c", "return a|pp();");
        expect(await files(s, "textDocument/definition", call)).toEqual(["ws/shared/app.c"]);
        expect(await files(s, "textDocument/declaration", call)).toEqual([
            "ws/shared/app.c",
            "ws/shared/app.h",
        ]);
        expect(
            await files(s, "textDocument/references", call, {
                context: { includeDeclaration: true },
            }),
        ).toEqual(["ws/main.c", "ws/shared/app.c", "ws/shared/app.h"]);
    },
);
