/// workspace/symbol ranks before it cuts: the exact name first, then names
/// starting with the query, then names merely containing it. The index's
/// symbol table iterates in hash order, so without ranking a weak match
/// (`foo_093`) could take a slot of the result list from the exact `foo` —
/// and with the list capped, push it out entirely.

import { test, expect } from "../fixtures.ts";

test("exact name outranks prefix and substring matches", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write(
        "rank.cpp",
        [
            "int xfoo() { return 3; }",
            "int foobar() { return 2; }",
            "int foo() { return 1; }",
            "",
        ].join("\n"),
    );
    workspace.writeCDB(["rank.cpp"]);
    await client.initialize(workspace);

    const [uri] = client.open("rank.cpp");
    expect(await client.waitForIndex(uri, "foo"), "Index not ready after 30s").toBe(true);

    const result = await client.workspaceSymbols("foo");
    expect(result).not.toBeNull();
    expect(result!.map((s) => s.name)).toEqual(["foo", "foobar", "xfoo"]);
});

test("paths start at the workspace root", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("src/a.cpp", "int sum_src() { return 1; }\n");
    workspace.write("lib/src/b.cpp", "int sum_lib() { return 2; }\n");
    workspace.writeCDB(["src/a.cpp", "lib/src/b.cpp"]);
    await client.initialize(workspace);

    const [uri] = client.open("src/a.cpp");
    expect(await client.waitForIndex(uri, "sum_lib"), "Index not ready after 30s").toBe(true);

    const named = async (text: string) =>
        ((await client.workspaceSymbols(text)) ?? []).map((s) => s.name).sort();
    expect(await named("sum path:src/")).toEqual(["sum_src"]);
    expect(await named("sum path:lib/src/")).toEqual(["sum_lib"]);
    expect(await named("sum path:b.cpp")).toEqual(["sum_lib"]);
});
