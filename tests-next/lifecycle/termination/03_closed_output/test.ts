import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// An editor that stops reading the server: the reply cannot be written,
// and the server ends instead of blocking.

test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    // The editor's side of the pipe, which no action models.
    s.client.child.stdout.destroy();
    void s.client
        .sendRequest(proto.WorkspaceSymbolRequest.type, { query: "main" })
        .catch(() => undefined);
    await s.client.assertExitedCleanly(15_000);
    s.client.dispose();
});
