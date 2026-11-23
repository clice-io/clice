import { answers } from "@clice/tools/testing";

const KINDS = [
    "refactor.rewrite.generateMemberwiseConstructor",
    "refactor.rewrite.declareSpecialMembers",
    "refactor.rewrite.implementPureVirtuals",
    "refactor.rewrite.define",
];

answers(async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.codeActions(m, { only: KINDS, compile: true }));
    }
});
