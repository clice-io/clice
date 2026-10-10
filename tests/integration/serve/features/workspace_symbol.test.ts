/// workspace/symbol ranks before it cuts: the exact name first, then names
/// starting with the query, then names merely containing it. The index's
/// symbol table iterates in hash order, so without ranking a weak match
/// (`foo_093`) could take a slot of the result list from the exact `foo` —
/// and with the list capped, push it out entirely.

import { expect, serve } from "../../fixtures.ts";

serve.files({
    "rank.cpp": "int xfoo() { return 3; }\nint foobar() { return 2; }\nint foo() { return 1; }\n",
})("exact name outranks prefix and substring matches", async ({ s }) => {
    await s.compiled("rank.cpp");
    await s.indexed();

    const result = await s.workspaceSymbols("foo");
    expect(result).not.toBeNull();
    expect(result!.map((symbol) => symbol.name)).toEqual(["foo", "foobar", "xfoo"]);
});
