/// Initialize must tolerate unknown enum values and fields from newer clients.
///
/// The three hostile parameter payloads are captured from the generative
/// builder (tests/tools/injection.py) that walks the full InitializeParams
/// tree and injects unknown string enum values, out-of-range integer enum
/// values, or unknown object fields. Each payload's `injected` count is the
/// builder's own tally, asserted here against the same floor.

import * as fs from "node:fs";
import * as proto from "vscode-languageserver-protocol";
import { expect, serve } from "../../fixtures.ts";

const INJECTION_FLOOR = 5;

interface HostileMode {
    injected: number;
    params: Record<string, unknown>;
}

const hostile = JSON.parse(
    fs.readFileSync(new URL("./hostile_init_params.json", import.meta.url), "utf8"),
) as Record<string, HostileMode>;

const MODES = ["unknown_string_enums", "out_of_range_int_enums", "unknown_fields"] as const;

serve("tiny", { launch: { handshake: false } }).for(MODES)(
    "initialize hostile params %s",
    async (mode, { s }) => {
        const { params, injected } = hostile[mode]!;
        expect(injected, `builder injected too little: ${injected}`).toBeGreaterThanOrEqual(
            INJECTION_FLOOR,
        );

        // The case's server is only spawned: it gets the hostile handshake.
        const wsUri = s.workspace.uri();
        const hostileParams = {
            ...params,
            processId: process.pid,
            rootPath: s.workspace.root,
            rootUri: wsUri,
            workspaceFolders: [{ uri: wsUri, name: "test" }],
            initializationOptions: { project: { cache_dir: s.workspace.path(".clice") } },
        };

        const result = (await s.client.sendRequest(
            "initialize",
            hostileParams,
        )) as proto.InitializeResult;
        expect(result.serverInfo?.name).toBe("clice");
        expect(result.capabilities).toBeDefined();

        await s.client.sendNotification(proto.InitializedNotification.type, {});
        // Teardown gates a clean shutdown/exit — the whole handshake must survive
        // the hostile params.
    },
);
