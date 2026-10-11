import { expect } from "vitest";
import { test, type Serve, type TestContext } from "@clice/tools/testing";

function databases(t: TestContext): void {
    t.disk.database({ "alpha/shared.cpp": ["-DFIRST"] }, "alpha");
    t.disk.database({ "alpha/shared.cpp": ["-DSECOND"] }, "beta");
}

/// The command hashes of the file's own entries, its owner's first.
async function own(s: Serve): Promise<string[]> {
    const listed = await s.contexts("alpha/shared.cpp");
    return listed.contexts
        .filter((context) => s.relative(context.uri) === "alpha/shared.cpp")
        .map((context) => context.commandHash ?? "");
}

test("own configuration of another folder", async (t) => {
    databases(t);
    const s = await t.start({ folders: ["alpha", "beta"] });
    expect(await s.errors("alpha/shared.cpp")).toEqual([]);
    // Its owner's entry comes first; the other project's can be switched
    // to, moving the file there.
    await t.show("entries", s.contexts("alpha/shared.cpp"));
    const [, other] = await own(s);
    const switched = await s.switchContext("alpha/shared.cpp", "alpha/shared.cpp", {
        commandHash: other ?? "",
    });
    expect(switched.success).toBe(true);
    await t.show("switched", s.currentContext("alpha/shared.cpp"));
});

test("a closed file keeps its choice", async (t) => {
    databases(t);
    const s = await t.start({ folders: ["alpha", "beta"] });
    expect(await s.errors("alpha/shared.cpp")).toEqual([]);
    const [first, other] = await own(s);
    expect(
        (
            await s.switchContext("alpha/shared.cpp", "alpha/shared.cpp", {
                commandHash: other ?? "",
            })
        ).success,
    ).toBe(true);
    s.close("alpha/shared.cpp");
    // A closed file's choice is not switched.
    expect(
        (
            await s.switchContext("alpha/shared.cpp", "alpha/shared.cpp", {
                commandHash: first ?? "",
            })
        ).success,
    ).toBe(false);
    expect(await s.errors("alpha/shared.cpp")).toEqual([]);
    await t.show("reopened", s.currentContext("alpha/shared.cpp"));
});

test("shared source listed once", async (t) => {
    databases(t);
    const s = await t.start({ folders: ["alpha", "beta"] });
    await s.indexed();
    await t.show("symbols", s.workspaceSymbols("shared"));
});
