import { expect } from "vitest";
import * as proto from "vscode-languageserver-protocol";
import { type Serve, test } from "@clice/tools/testing";

// An editor whose only UI for contexts is its code action menu (Zed, Helix)
// switches them through source actions, whose commands the server runs.
// The actions are offered when the user asks for actions, not on the
// requests an editor sends as the cursor moves.

function actions(
    s: Serve,
    file: string,
    triggerKind: proto.CodeActionTriggerKind = proto.CodeActionTriggerKind.Invoked,
): Promise<proto.CodeAction[]> {
    const start = { line: 0, character: 0 };
    return s.client.sendRequest(proto.CodeActionRequest.type, {
        textDocument: { uri: s.uri(file) },
        range: { start, end: start },
        context: { diagnostics: [], only: [proto.CodeActionKind.Source], triggerKind },
    }) as Promise<proto.CodeAction[]>;
}

async function run(s: Serve, offered: proto.CodeAction[], title: string): Promise<unknown> {
    const command = offered.find((action) => action.title === title)?.command;
    if (command === undefined) {
        throw new Error(`no action ${title}`);
    }
    return s.client.sendRequest(proto.ExecuteCommandRequest.type, {
        command: command.command,
        arguments: command.arguments ?? [],
    });
}

const titles = (offered: proto.CodeAction[]) => offered.map((action) => action.title);

test(async (t) => {
    const s = await t.start();
    expect(await s.errors("shared.h")).toEqual([]);
    expect(await actions(s, "shared.h", proto.CodeActionTriggerKind.Automatic)).toEqual([]);
    const offered = await actions(s, "shared.h");
    await t.show("offered", titles(offered));
    expect(await run(s, offered, "Compile as included by real_host.cpp")).toEqual({
        success: true,
        stale: false,
    });
    await t.show("switched", s.currentContext("shared.h"));
    const chosen = await actions(s, "shared.h");
    await t.show("offered while chosen", titles(chosen));
    expect(await run(s, chosen, "Compile in the context clice picks")).toEqual({
        success: true,
        stale: false,
    });
    await t.show("reset", s.currentContext("shared.h"));
});
