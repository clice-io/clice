import { answers } from "@clice/tools/testing";

answers(async (a) => {
    const marks = a.markers();
    if (marks.length === 0) {
        await a.show("main.cpp", a.inlayHints("main.cpp"));
    }
    for (const m of marks) {
        await a.show(m.name, a.inlayHints(m));
    }
});
