import { answers } from "@clice/tools/testing";

// clice.toml appends -fdelayed-template-parsing to every command. scaled()
// names `factor` before its declaration, which compiles only deferred:
// instantiated at the end of the file, its body sees it. shifted() is
// never instantiated, so its body stays unparsed.

answers(async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.hover(m));
    }
});
