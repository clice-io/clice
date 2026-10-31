import * as proto from "vscode-languageserver-protocol";
import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The application's source, the header's chosen host in another folder, is
// deleted, and the next request on the header is the first to look: the
// choice stops holding while that request waits, which answers ContentModified
// for the editor to ask again, and the header goes back to its own folder's
// host.
test(async (t) => {
    const s = await t.start({ folders: ["app", "lib"] });
    expect(await s.errors("lib/include/lib.h")).toHaveLength(1);
    expect((await s.switchContext("lib/include/lib.h", "app/main.cpp")).success).toBe(true);
    expect(await s.errors("lib/include/lib.h")).toEqual([]);
    t.disk.rm("app/main.cpp");
    await expect(s.hover(t.at("lib/include/lib.h", "int |lib_fn"))).rejects.toMatchObject({
        code: proto.LSPErrorCodes.ContentModified,
    });
    await t.show("host removed", {
        context: s.currentContext("lib/include/lib.h"),
        errors: s.errors("lib/include/lib.h"),
    });
});
