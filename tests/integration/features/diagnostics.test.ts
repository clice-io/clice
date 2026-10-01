/// Published diagnostics: where each one lands in the document, the notes
/// it carries, and which diagnostics of other files reach a document.

import type * as proto from "vscode-languageserver-protocol";
import type { CliceClient } from "@clice/tools/client";
import { expect, test } from "../fixtures.ts";

function published(client: CliceClient, uri: string): proto.Diagnostic[] {
    return client.diagnostics.get(uri) ?? [];
}

function withCode(client: CliceClient, uri: string, code: string): proto.Diagnostic[] {
    return published(client, uri).filter((diagnostic) => diagnostic.code === code);
}

function span(range: proto.Range): string {
    return `${range.start.line}:${range.start.character}-${range.end.line}:${range.end.character}`;
}

/// Related information as `file@range message`, the file by its name.
function related(client: CliceClient, diagnostic: proto.Diagnostic): string[] {
    return (diagnostic.relatedInformation ?? []).map((info) => {
        const file = client.normalizeUri(info.location.uri).split("/").pop();
        return `${file}@${span(info.location.range)} ${info.message}`;
    });
}

test("notes become related information", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", "int a = 1;\nint a = 2;\nvoid f(int);\nvoid g() { f(1, 2); }\n");
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");

    const [redefinition] = withCode(client, uri, "err_redefinition");
    expect(span(redefinition!.range)).toBe("1:4-1:5");
    expect(related(client, redefinition!)).toEqual([
        "main.cpp@0:4-0:5 previous definition is here",
    ]);
    const [call] = withCode(client, uri, "err_ovl_no_viable_function_in_call");
    expect(related(client, call!)).toEqual([
        "main.cpp@2:5-2:6 candidate function not viable: requires 1 argument, but 2 were provided",
    ]);
});
