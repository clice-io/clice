import * as proto from "vscode-languageserver-protocol";
import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("document without a file is not served", async (t) => {
    const s = await t.start();
    // The actions name workspace files: an untitled buffer goes raw.
    const untitled = "untitled:Untitled-1";
    await s.client.sendNotification(proto.DidOpenTextDocumentNotification.type, {
        textDocument: { uri: untitled, languageId: "cpp", version: 0, text: "int main() {}\n" },
    });
    const hover = await s.client.hoverAt(untitled, 0, 5).then(
        () => "answered",
        (error: unknown) => (error instanceof Error ? error.message : String(error)),
    );
    await t.show("hover", hover);
    // No place in the index.
    expect(await s.client.referencesAt(untitled, 0, 5)).toEqual([]);
    // The server keeps serving files.
    expect(await s.errors("main.cpp")).toEqual([]);
});
