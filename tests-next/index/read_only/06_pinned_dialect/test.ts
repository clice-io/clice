import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A file with two commands is lexed for the one its context pins: switched
// to the C command, `class` is no keyword and goes unpainted.
test(async (t) => {
    const s = await t.start({ config: { project: { readonly: "on" } } });
    s.open("dual.c", { pull: false });
    await s.sync();
    await t.show("contexts", s.contexts("dual.c"));
    await t.show("first", s.semanticTokens("dual.c"));
    const { contexts } = await s.contexts("dual.c");
    const commandHash = contexts.find((context) =>
        context.label.includes("DIALECT_C"),
    )?.commandHash;
    if (commandHash === undefined) {
        throw new Error("no context of dual.c names DIALECT_C");
    }
    expect(await s.switchContext("dual.c", "dual.c", { commandHash })).toEqual({
        success: true,
        stale: false,
    });
    await t.show("switched", s.semanticTokens("dual.c"));
});
