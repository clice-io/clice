import * as proto from "vscode-languageserver-protocol";
import { expect } from "vitest";
import { refusal, test } from "@clice/tools/testing";

// `clice serve --workspace` loads its workspace before any client speaks;
// the guidance that load emits is shown once the client's handshake ends,
// window/showMessage being no message to send ahead of it, and the missing
// database once a file of the workspace opens.

test("shown after the handshake", async (t) => {
    const s = await t.spawn({ args: ["serve", `--workspace=${t.root}`] });
    await t.show("before the handshake", await s.guidance());
    await refusal(
        s.client.sendRequest("initialize", {
            processId: null,
            rootUri: s.uri(""),
            capabilities: {},
        }),
    );
    await s.client.sendNotification(proto.InitializedNotification.method, {});
    await t.show("after the handshake", await s.guidance());
    s.open("main.cpp", { pull: false });
    await t.show("guidance", await s.guidance());
});

// A database the build writes after the start, before the first file
// opens, is no database missing.
test("a database written before a file opens", async (t) => {
    const s = await t.start();
    await s.sync();
    t.disk.database({ "main.cpp": [] });
    expect(await s.poll("cdb")).toBe(1);
    s.open("main.cpp", { pull: false });
    expect(await s.guidance()).toEqual([]);
});
