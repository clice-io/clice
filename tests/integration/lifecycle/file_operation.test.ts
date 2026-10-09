/// File operation tests for the clice LSP server.

import { IDLE_TIMEOUT, sleep } from "@clice/tools/client";
import { test, expect } from "../fixtures.ts";

const EDIT_INTERVAL = 200;

test("did open", async ({ session }) => {
    const { client } = await session("hello_world");
    client.open("main.cpp");
    await sleep(IDLE_TIMEOUT);
});

test("did change", async ({ session }) => {
    const { client } = await session("hello_world");
    const [uri, initial] = client.open("main.cpp");
    let content = initial;
    for (let i = 0; i < 20; i++) {
        content += "\n";
        await sleep(EDIT_INTERVAL);
        client.change(uri, i + 1, content);
    }
    await sleep(IDLE_TIMEOUT);
});

test("clang tidy", async ({ session }) => {
    const { client } = await session("clang_tidy");
    client.open("main.cpp");
    await sleep(IDLE_TIMEOUT);
});

test("hover save close", async ({ session }) => {
    const { client } = await session("hello_world");
    const [uri] = client.open("main.cpp");
    const hover = await client.hoverAt(uri, 2, 4);
    expect(hover).not.toBeNull();
    expect(hover!.contents).not.toBeNull();
    await client.completionAt(uri, 0, 0);
    await client.signatureHelpAt(uri, 0, 0);
    client.close(uri);
    await expect(client.hoverAt(uri, 0, 0)).rejects.toThrow("Document not open");
});
