/// Lifecycle tests for the clice LSP server.

import * as proto from "vscode-languageserver-protocol";
import { expect, serve } from "../../fixtures.ts";

const test = serve.data("hello_world");

test("initialize", ({ s }) => {
    const { initResult } = s;
    expect(initResult.serverInfo).not.toBeUndefined();
    expect(initResult.serverInfo!.name).toBe("clice");
});

test("double initialize rejected", async ({ s }) => {
    await expect(
        s.client.sendRequest(proto.InitializeRequest.type, {
            processId: null,
            rootUri: null,
            capabilities: {},
            workspaceFolders: [],
        }),
    ).rejects.toThrow();
});

test("shutdown", async ({ s }) => {
    await s.client.sendRequest(proto.ShutdownRequest.type);
});
