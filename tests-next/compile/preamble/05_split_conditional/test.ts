import { answers } from "@clice/tools/testing";

answers(async (a) => {
    await a.show("other", a.hover(a.marker("other")));
});
