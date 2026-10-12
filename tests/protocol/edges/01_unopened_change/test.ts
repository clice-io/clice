import { expect } from "vitest";
import * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// A change to a document never opened has no text to apply to: it is
// dropped, and the open that follows starts from the editor's text.

test({ errors: ["didChange for a document with no open session, dropping"] }, async (t) => {
    const s = await t.start();
    // Before any open: no action sends it.
    s.client.change(s.uri("main.cpp"), 1, "int broken(");
    const refused = await s.hover(t.marker("add")).then(
        () => null,
        (error: unknown) => error,
    );
    expect(refused).toBeInstanceOf(proto.ResponseError);
    const { code, message } = refused as proto.ResponseError<unknown>;
    await t.show("hover", { code, message });
    await t.show("diagnostics after an open", s.diagnostics("main.cpp"));
});
