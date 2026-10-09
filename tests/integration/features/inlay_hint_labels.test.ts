/// How an inlay hint label naming a symbol reaches each kind of client: one
/// string for a client without LSP 3.17 inlay hints, pieces with their
/// locations for one with them. The snap corpus pins the located form.

import * as proto from "vscode-languageserver-protocol";
import type { CliceClient } from "@clice/tools/client";
import type { SessionFactory } from "@clice/tools/session";
import { expect, test } from "../fixtures.ts";

const WHOLE_FILE: proto.Range = {
    start: { line: 0, character: 0 },
    end: { line: 10, character: 0 },
};

async function hintsFor(
    session: SessionFactory,
    capabilities: proto.ClientCapabilities,
): Promise<{ client: CliceClient; hints: proto.InlayHint[] }> {
    const workspace = session.tmpdir();
    workspace.write("widget.h", "#pragma once\n\nstruct Widget;\n\nstruct Widget {};\n");
    workspace.write(
        "main.cpp",
        '#include "widget.h"\n\nWidget make();\n\nvoid use() {\n    auto widget = make();\n}\n',
    );
    workspace.writeCDB(["main.cpp"], { std: "c++23" });
    const client = session.spawn(workspace);
    await client.initialize(workspace, { capabilities });
    const [uri] = await client.openAndWait("main.cpp");
    const hints = (await client.inlayHints(uri, WHOLE_FILE)) ?? [];
    expect(hints.length).toBe(1);
    return { client, hints };
}

function parts(hint: proto.InlayHint): proto.InlayHintLabelPart[] {
    expect(typeof hint.label).not.toBe("string");
    return hint.label as proto.InlayHintLabelPart[];
}

/// The forward declaration in widget.h: go-to-definition there reaches the
/// definition.
function expectWidgetDeclaration(part: proto.InlayHintLabelPart) {
    expect(part.value).toBe("Widget");
    expect(part.location?.uri.endsWith("/widget.h")).toBe(true);
    expect(part.location?.range).toEqual({
        start: { line: 2, character: 7 },
        end: { line: 2, character: 13 },
    });
}

test("plain label without inlay hint support", async ({ session }) => {
    const { hints } = await hintsFor(session, {});
    expect(hints[0]!.label).toBe(": Widget");
});

test("located parts with the hints", async ({ session }) => {
    const { client, hints } = await hintsFor(session, { textDocument: { inlayHint: {} } });
    const [prefix, widget] = parts(hints[0]!);
    expect(prefix).toEqual({ value: ": " });
    expectWidgetDeclaration(widget!);
    expect(client.initResult!.capabilities.inlayHintProvider).toBe(true);
});
