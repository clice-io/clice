/// Lifecycle tests for the clice LSP server.

import * as proto from "vscode-languageserver-protocol";
import { cliceTest, expect } from "../../tools/fixtures.ts";

const test = cliceTest("hello_world");

test("initialize", ({ client }) => {
    expect(client.initResult).not.toBeNull();
    expect(client.initResult!.serverInfo).not.toBeUndefined();
    expect(client.initResult!.serverInfo!.name).toBe("clice");
});

test("double initialize rejected", async ({ client }) => {
    await expect(
        client.connection.sendRequest(proto.InitializeRequest.type, {
            processId: null,
            rootUri: null,
            capabilities: {},
            workspaceFolders: [],
        }),
    ).rejects.toThrow();
});

test("shutdown", async ({ client }) => {
    await client.connection.sendRequest(proto.ShutdownRequest.type);
});
