import { expect } from "vitest";
import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// A call hierarchy item is the client's to keep: it comes back on every
// incomingCalls and outgoingCalls, and the server finds its symbol again
// through the index. These send items the server never made, so they go
// through the raw client.
test("item without its data", async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    await s.indexed();
    const m = t.marker("add");
    await t.show("incoming", s.incomingCalls(m));
    const { uri, position } = s.position(m);
    const [item] =
        (await s.client.prepareCallHierarchy(uri, position.line, position.character)) ?? [];
    if (item?.data === undefined) {
        throw new Error("the call hierarchy item at add carries no data");
    }
    const stripped: proto.CallHierarchyItem = { ...item };
    delete stripped.data;
    expect(await s.client.callHierarchyIncoming(stripped)).toEqual(
        await s.client.callHierarchyIncoming(item),
    );
});

test("item of no symbol", async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    await s.indexed();
    const ghost: proto.CallHierarchyItem = {
        name: "ghost",
        kind: proto.SymbolKind.Function,
        uri: s.uri("ghost.cpp"),
        range: { start: { line: 0, character: 0 }, end: { line: 0, character: 5 } },
        selectionRange: { start: { line: 0, character: 0 }, end: { line: 0, character: 5 } },
    };
    const error = await s.client.callHierarchyIncoming(ghost).then(
        () => "answered",
        (failure: unknown) => (failure instanceof Error ? failure.message : String(failure)),
    );
    await t.show("incoming", error);
});
