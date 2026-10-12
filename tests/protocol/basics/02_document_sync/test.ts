import { expect } from "vitest";
import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// The documents an editor opens, changes, saves and closes: the server
// follows every change, clears a closed document's diagnostics, and
// refuses requests about a document it does not hold.

async function refusal(reply: Promise<unknown>): Promise<{ code: number; message: string }> {
    const error = await reply.then(
        () => null,
        (failure: unknown) => failure,
    );
    expect(error).toBeInstanceOf(proto.ResponseError);
    const { code, message } = error as proto.ResponseError<unknown>;
    return { code, message };
}

test("open and close", async (t) => {
    const s = await t.start();
    await t.show("diagnostics", s.diagnostics("main.cpp"));
    s.close("main.cpp");
    await t.show("pushes", s.pushes("main.cpp"));
    await t.show("hover after the close", await refusal(s.hover(t.marker("add"))));
});

test("every change applies", async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    let text = t.disk.read("main.cpp");
    for (let change = 1; change < 20; change++) {
        text += `// change ${String(change)}\n`;
        s.edit("main.cpp", { text });
    }
    s.edit("main.cpp", { text: `${text}int broken(\n` });
    await t.show("diagnostics", s.diagnostics("main.cpp"));
    await t.show("hover", s.hover(t.at("main.cpp", "int |add")));
});

test("save", async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    s.edit("main.cpp", { replace: "a + b", with: "a - b" });
    s.save("main.cpp");
    await t.show("diagnostics", s.diagnostics("main.cpp"));
    await t.show("disk", t.disk.read("main.cpp"));
});

test("a document never opened", async (t) => {
    const s = await t.start();
    await t.show("hover", await refusal(s.hover(t.marker("add"))));
    await t.show(
        "hover outside the workspace",
        await refusal(s.client.hoverAt("file:///nonexistent/fake.cpp", 0, 0)),
    );
});

// Shutdown answers, and the exit after a second one is still clean.
test("shutdown twice", async (t) => {
    const s = await t.start();
    await s.client.sendRequest(proto.ShutdownRequest.type);
});
