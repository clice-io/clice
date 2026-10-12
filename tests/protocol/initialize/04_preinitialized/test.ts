import { expect } from "vitest";
import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// `clice serve --workspace` is ready before any client speaks. What it
// computes meanwhile is not pushed ahead of the handshake; the handshake's
// initialized replays it, unless the document changed since, and the
// initialize request itself is refused, the server being initialized
// already.

async function refusal(reply: Promise<unknown>): Promise<{ code: number; message: string }> {
    const error = await reply.then(
        () => null,
        (failure: unknown) => failure,
    );
    expect(error).toBeInstanceOf(proto.ResponseError);
    const { code, message } = error as proto.ResponseError<unknown>;
    return { code, message };
}

/// The handshake a client sends a server that needs none: raw messages,
/// the refusal being the point.
async function handshake(
    client: {
        sendRequest(method: string, params: unknown): Promise<unknown>;
        sendNotification(method: string, params: unknown): Promise<void>;
    },
    rootUri: string,
): Promise<{ code: number; message: string }> {
    const refused = await refusal(
        client.sendRequest("initialize", { processId: null, rootUri, capabilities: {} }),
    );
    await client.sendNotification(proto.InitializedNotification.method, {});
    return refused;
}

test("the handshake replays", async (t) => {
    const s = await t.spawn({ args: ["serve", `--workspace=${t.root}`] });
    s.open("main.cpp", { pull: false });
    await t.show("hover", s.hover(t.marker("add")));
    // An ungated push goes out during the compile the hover waits for.
    expect(s.client.publishCount(s.uri("main.cpp"))).toBe(0);
    await t.show("initialize", await handshake(s.client, s.uri("")));
    await t.show("pushes", s.pushes("main.cpp"));
});

test("an edit drops the replay", async (t) => {
    const s = await t.spawn({ args: ["serve", `--workspace=${t.root}`] });
    s.open("main.cpp", { pull: false });
    await t.show("hover", s.hover(t.marker("add")));
    s.edit("main.cpp", { after: "}\n", insert: "int bad(\n" });
    await t.show("initialize", await handshake(s.client, s.uri("")));
    // A later reply orders this after the initialized: a replay of the
    // output before the edit would have been pushed by now.
    await s.contexts("main.cpp");
    expect(s.client.publishCount(s.uri("main.cpp"))).toBe(0);
    await t.show("diagnostics", s.diagnostics("main.cpp"));
});
