import { answers, test, type AnswersContext } from "@clice/tools/testing";

async function items(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["documentation"] });
    }
}

answers("markdown client", items);

// A client that does not take markdown gets the documentation as plain text.
test("plain text client", async (t) => {
    const s = await t.start({
        capabilities: {
            textDocument: {
                completion: { completionItem: { documentationFormat: ["plaintext"] } },
            },
        },
    });
    s.open("main.cpp");
    await s.sync();
    await t.show("function", s.completion(t.marker("function")), {
        parts: ["documentation"],
    });
});
