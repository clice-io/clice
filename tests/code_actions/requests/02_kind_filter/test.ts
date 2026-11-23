import { answers } from "@clice/tools/testing";

const KINDS = [
    "quickfix",
    "refactor",
    "refactor.rewrite.define",
    "refactor.rewrite.define.outOfLine",
    "refactor.rewrite.def",
    "refactor.rewrite.populateSwitch",
];

// A kind matches itself and the kinds below it, never a prefix of a word.
answers(async (a) => {
    const f = a.marker("f");
    await a.show("all", a.codeActions(f));
    for (const kind of KINDS) {
        await a.show(kind, a.codeActions(f, { only: [kind] }));
    }
});
