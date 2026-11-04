import { answers } from "@clice/tools/testing";

answers(async (a) => {
    await a.show("member", a.completion(a.marker("member")));
    await a.show("call", a.hover(a.marker("call")));
});
