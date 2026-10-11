import { expect } from "vitest";
import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// What the server says about itself at the handshake, to the most
// conservative client.

test("capabilities", async (t) => {
    const s = await t.start();
    // The handshake's raw reply, which no action renders.
    const result = s.client.initResult;
    await t.show("capabilities", JSON.stringify(result?.capabilities, null, 4));
    await t.show("server", { name: result?.serverInfo?.name ?? "" });
    // The version is the build's: the handshake names the one the command
    // line prints.
    const run = await t.cli("--version");
    expect(result?.serverInfo?.version).toBe(/^clice version (\S+)/.exec(run.stdout)?.[1]);
});

test("a second initialize", async (t) => {
    const s = await t.start();
    const refused = await s.client
        .sendRequest(proto.InitializeRequest.type, {
            processId: null,
            rootUri: null,
            capabilities: {},
            workspaceFolders: [],
        })
        .then(
            () => null,
            (error: unknown) => error,
        );
    expect(refused).toBeInstanceOf(proto.ResponseError);
    const { code, message } = refused as proto.ResponseError<unknown>;
    await t.show("refused", { code, message });
});
