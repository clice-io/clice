import { expect } from "vitest";
import { test, type Serve, type TestContext } from "@clice/tools/testing";

/// The files a definition names, as the server spells them.
async function targets(s: Serve, at: ReturnType<TestContext["at"]>): Promise<string[]> {
    const reply = await s.request<{ uri: string }[]>("textDocument/definition", at);
    return reply.map(({ uri }) => decodeURIComponent(uri));
}

test(
    "folder opened through a symlink",
    { except: { win32: "a symlink on Windows needs a privilege the CI runners lack" } },
    async (t) => {
        t.disk.symlink("ws", "real");
        const s = await t.start({ folders: ["ws"] });
        expect(await s.errors("ws/main.cpp")).toEqual([]);
        // Rendering resolves links: the spelling is compared raw. Results
        // name a file the way the folder was opened, in the preamble and
        // past it.
        expect(await targets(s, t.at("ws/main.cpp", '#include "|early.h"'))).toEqual([
            s.uri("ws/inc/early.h"),
        ]);
        expect(await targets(s, t.at("ws/main.cpp", '#include "|late.h"'))).toEqual([
            s.uri("ws/inc/late.h"),
        ]);
        await t.show("hover", s.hover(t.at("ws/main.cpp", '#include "|late.h"')));
    },
);
