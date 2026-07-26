/// Initialize must tolerate unknown enum values and fields from newer clients.
///
/// The three hostile parameter payloads are captured from the generative
/// builder (tests/tools/injection.py) that walks the full InitializeParams
/// tree and injects unknown string enum values, out-of-range integer enum
/// values, or unknown object fields. Each payload's `injected` count is the
/// builder's own tally, asserted here against the same floor.

import * as fs from "node:fs";
import * as path from "node:path";
import * as proto from "vscode-languageserver-protocol";
import { URI } from "vscode-uri";
import { withTimeout } from "../../tools/client.ts";
import { writeCdb } from "../../tools/compile_commands.ts";
import { expect, test } from "../../tools/fixtures.ts";

const INJECTION_FLOOR = 5;

interface HostileMode {
    injected: number;
    params: Record<string, unknown>;
}

const hostile = JSON.parse(
    fs.readFileSync(new URL("./hostile_init_params.json", import.meta.url), "utf8"),
) as Record<string, HostileMode>;

const MODES = ["unknown_string_enums", "out_of_range_int_enums", "unknown_fields"] as const;

test.for(MODES)("initialize hostile params %s", async (mode, { session }) => {
    const { params, injected } = hostile[mode]!;
    expect(injected, `builder injected too little: ${injected}`).toBeGreaterThanOrEqual(
        INJECTION_FLOOR,
    );

    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "main.cpp"), "int main() { return 0; }\n");
    writeCdb(workspace, ["main.cpp"]);

    const wsUri = URI.file(workspace).toString();
    const hostileParams = {
        ...params,
        processId: 12345,
        rootPath: workspace,
        rootUri: wsUri,
        workspaceFolders: [{ uri: wsUri, name: "test" }],
        initializationOptions: { project: { cache_dir: path.join(workspace, ".clice") } },
    };

    const result = await withTimeout(
        client.connection.sendRequest<proto.InitializeResult>("initialize", hostileParams),
        30_000,
        "initialize",
    );
    expect(result.serverInfo?.name).toBe("clice");
    expect(result.capabilities).toBeDefined();

    await client.connection.sendNotification(proto.InitializedNotification.type, {});
    // Teardown gates a clean shutdown/exit — the whole handshake must survive
    // the hostile params.
});
