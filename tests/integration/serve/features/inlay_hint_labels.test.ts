/// How an inlay hint label naming a symbol reaches each kind of client: one
/// string for a client without LSP 3.17 inlay hints, pieces with their
/// locations for one with them. The snap corpus pins the located form.

import type * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import { expect, serve } from "../../fixtures.ts";

const WHOLE_FILE: proto.Range = {
    start: { line: 0, character: 0 },
    end: { line: 10, character: 0 },
};

const test = serve.files({
    "widget.h": "#pragma once\n\nstruct Widget;\n\nstruct Widget {};\n",
    "main.cpp":
        '#include "widget.h"\n\nWidget make();\n\nvoid use() {\n    auto widget = make();\n}\n',
});

/// A client declaring inlay hint support: the serve fixture's server is
/// started without it, so this one is a server of its own.
async function locatedHints(s: Serve): Promise<{ provider: unknown; hints: proto.InlayHint[] }> {
    await s.stop();
    const client = await s.session.spawn(s.workspace).initialize(s.workspace, {
        capabilities: { textDocument: { inlayHint: {} } },
    });
    const [uri] = client.open("main.cpp");
    await client.pullDiagnostics(uri);
    const hints = (await client.inlayHints(uri, WHOLE_FILE)) ?? [];
    expect(hints.length).toBe(1);
    return { provider: client.initResult!.capabilities.inlayHintProvider, hints };
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

test("plain label without inlay hint support", async ({ s }) => {
    await s.compiled("main.cpp");
    const hints = (await s.request("textDocument/inlayHint", "main.cpp", {
        range: WHOLE_FILE,
    })) as proto.InlayHint[] | null;
    expect(hints?.length).toBe(1);
    expect(hints![0]!.label).toBe(": Widget");
});

test("located parts with the hints", async ({ s }) => {
    const { provider, hints } = await locatedHints(s);
    const [prefix, widget] = parts(hints[0]!);
    expect(prefix).toEqual({ value: ": " });
    expectWidgetDeclaration(widget!);
    expect(provider).toBe(true);
});
