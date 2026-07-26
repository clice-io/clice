/// Lifecycle tests for the clice LSP server.

import * as proto from "vscode-languageserver-protocol";
import { cliceTest, expect } from "../../fixtures.ts";

const test = cliceTest("hello_world");

test("initialize", ({ client }) => {
    expect(client.initResult).not.toBeNull();
    expect(client.initResult!.serverInfo).not.toBeUndefined();
    expect(client.initResult!.serverInfo!.name).toBe("clice");
});

test("double initialize rejected", async ({ client }) => {
    await expect(
        client.sendRequest(proto.InitializeRequest.type, {
            processId: null,
            rootUri: null,
            capabilities: {},
            workspaceFolders: [],
        }),
    ).rejects.toThrow();
});

test("shutdown", async ({ client }) => {
    await client.sendRequest(proto.ShutdownRequest.type);
});
