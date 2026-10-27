import * as proto from "vscode-languageserver-protocol";
import { refusal, test } from "@clice/tools/testing";

// `clice serve --workspace` loads its workspace before any client speaks;
// the guidance that load emits is shown once the client's handshake ends,
// window/showMessage being no message to send ahead of it.

test(async (t) => {
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
    await t.show("guidance", await s.guidance());
});
