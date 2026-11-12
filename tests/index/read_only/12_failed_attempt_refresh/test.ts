import { test } from "@clice/tools/testing";

// scratch.cpp is in the database but only an editor buffer holds it, so its
// index attempt fails and "auto" turns it to compiling. Its tokens and
// folds were pulled while the attempt was held, and answered empty: the
// server asks the editor to pull them again.
const CAPABILITIES = {
    workspace: {
        semanticTokens: { refreshSupport: true },
        foldingRange: { refreshSupport: true },
    },
};

test(async (t) => {
    t.disk.database({ "main.cpp": [], "scratch.cpp": [] });
    const s = await t.start({
        config: { project: { readonly: "auto" } },
        capabilities: CAPABILITIES,
    });
    await s.sync();
    const held = await s.park("index", "scratch.cpp", { at: "reply" });
    s.open("scratch.cpp", { text: "int scratch() {\n    return 2;\n}\n", pull: false });
    await t.show("held tokens", s.semanticTokens("scratch.cpp"));
    await t.show("held folds", s.foldingRanges("scratch.cpp"));
    await held.reached();
    await held.release();
    await t.show("refreshes", {
        semantic_tokens: await s.serverRequests("workspace/semanticTokens/refresh"),
        folding_range: await s.serverRequests("workspace/foldingRange/refresh"),
    });
    await t.show("pulled again", s.semanticTokens("scratch.cpp"));
});
