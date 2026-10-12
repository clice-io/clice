import type * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// The answers of other cases render each link's target; this one shows the
// tooltip an editor puts beside the follow-link hint.
test(async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    await t.show("main.cpp", s.documentLinks("main.cpp"));
    const links = await s.request<proto.DocumentLink[] | null>(
        "textDocument/documentLink",
        "main.cpp",
    );
    await t.show(
        "tooltips",
        (links ?? []).map((link) => link.tooltip ?? "(none)"),
    );
});
