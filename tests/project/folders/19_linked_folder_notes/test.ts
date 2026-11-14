import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const RELATED = { textDocument: { publishDiagnostics: { relatedInformation: true } } };

// The folder is opened through a link to the directory holding the files:
// a diagnostic's notes name the files as the folder spells them, as the
// diagnostics themselves do.
test(async (t) => {
    t.disk.linkDirectory("ws", "real");
    const s = await t.start({ folders: ["ws"], capabilities: RELATED });
    await t.show("main.cpp", s.diagnostics("ws/main.cpp"), { parts: ["related"] });
    // Rendering resolves links: the spelling is compared raw.
    const notes = [...s.client.diagnostics]
        .filter(([uri]) => s.spelling(uri) === "ws/main.cpp")
        .flatMap(([, diagnostics]) => diagnostics)
        .flatMap((diagnostic) => diagnostic.relatedInformation ?? [])
        .map((info) => s.spelling(info.location.uri));
    expect(notes).toEqual(["ws/inc/a.h"]);
});
