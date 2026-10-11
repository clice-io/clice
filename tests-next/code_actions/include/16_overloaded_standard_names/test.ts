import { answers } from "@clice/tools/testing";

// No compile check: the claim is which headers are offered, and the case
// ships none of them.
answers(async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.codeActions(m));
    }
});
