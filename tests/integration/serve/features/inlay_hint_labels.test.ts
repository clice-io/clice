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

const FILES = {
    "widget.h": "#pragma once\n\nstruct Widget;\n\nstruct Widget {};\n",
    "main.cpp":
        '#include "widget.h"\n\nWidget make();\n\nvoid use() {\n    auto widget = make();\n}\n',
};

/// The one hint of main.cpp, compiled.
async function hintOf(s: Serve): Promise<proto.InlayHint> {
    await s.compiled("main.cpp");
    const hints = await s.request<proto.InlayHint[] | null>("textDocument/inlayHint", "main.cpp", {
        range: WHOLE_FILE,
    });
    expect(hints?.length).toBe(1);
    return hints![0]!;
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

serve.files(FILES)("plain label without inlay hint support", async ({ s }) => {
    expect((await hintOf(s)).label).toBe(": Widget");
});

serve.files(FILES, { launch: { capabilities: { textDocument: { inlayHint: {} } } } })(
    "located parts with the hints",
    async ({ s }) => {
        const [prefix, widget] = parts(await hintOf(s));
        expect(prefix).toEqual({ value: ": " });
        expectWidgetDeclaration(widget!);
        expect(s.initResult.capabilities.inlayHintProvider).toBe(true);
    },
);
