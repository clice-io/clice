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
    only: proto.CodeActionKind[] | undefined = [proto.CodeActionKind.Source],
): Promise<proto.CodeAction[]> {
    const start = { line: 0, character: 0 };
    return s.client.sendRequest(proto.CodeActionRequest.type, {
        textDocument: { uri: s.uri(file) },
        range: { start, end: start },
        context: { diagnostics: [], only, triggerKind },
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

test("switch and reset", async (t) => {
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

// Asked for every kind of action, the server answers once the file has
// compiled. A header's first compile finds it self-contained, which folds
// its hosts of the same flags into one context: nothing is left to switch to.
test("listed after the first compile", async (t) => {
    const s = await t.start();
    expect((await s.contexts("plain.h")).total).toBe(2);
    s.open("plain.h");
    const offered = await actions(s, "plain.h", proto.CodeActionTriggerKind.Invoked, undefined);
    expect(titles(offered.filter((action) => action.kind === "source.clice.context"))).toEqual([]);
    expect((await s.contexts("plain.h")).total).toBe(1);
});
