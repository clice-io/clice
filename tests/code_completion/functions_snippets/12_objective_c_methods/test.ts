import { answers, test, type AnswersContext } from "@clice/tools/testing";

async function snippets(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert"] });
    }
}

answers("default", snippets);
answers("delimiters", { config: { code_completion: { arguments: "delimiters" } } }, snippets);
answers("none", { config: { code_completion: { arguments: "none" } } }, snippets);

// The server's own client declares no snippet support.
test("plain text client", async (t) => {
    const s = await t.start();
    s.open("main.m");
    await s.sync();
    await t.show("message", s.completion(t.marker("message")), { parts: ["insert"] });
});
