import type { Diagnostic } from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// A compile's diagnostics are placed in the text it compiled: a push names
// that text's version, a pull answers the buffer as it is now. The edit
// moves the error by characters that take a different number of units in
// each encoding.

const QUIET = { diagnostics: { clang_tidy: false }, project: { enable_indexing: false } };
const PULL = { textDocument: { diagnostic: {} } };
const ENCODINGS = [undefined, "utf-8", "utf-32"] as const;
const EDIT = { replace: `"宽𝄞"`, with: `"宽𝄞宽𝄞宽𝄞"` };

/// Where the compiler's diagnostics start, as line:character.
function at(diagnostics: readonly Diagnostic[]): string[] {
    return diagnostics
        .filter(({ source }) => source === "clang")
        .map(({ range }) => `${String(range.start.line)}:${String(range.start.character)}`);
}

for (const positionEncoding of ENCODINGS) {
    const name = positionEncoding ?? "utf-16";
    const options = positionEncoding ? { positionEncoding } : {};

    test(`push ${name}`, async (t) => {
        const s = await t.start({ config: QUIET, ...options });
        s.open("main.cpp");
        await s.pushes("main.cpp");
        s.edit("main.cpp", EDIT);
        await s.hover(t.at("main.cpp", "int total = val|ue;"));
        await s.pushes("main.cpp");
        const uri = s.client.normalizeUri(s.uri("main.cpp"));
        await t.show(
            "pushes",
            s.client.publishedDiagnostics
                .filter((params) => s.client.normalizeUri(params.uri) === uri)
                .map(
                    ({ version, diagnostics }) =>
                        `version ${String(version)}: ${at(diagnostics).join(", ")}`,
                ),
        );
    });

    test(`pull ${name}`, async (t) => {
        const s = await t.start({ config: QUIET, capabilities: PULL, ...options });
        await s.diagnostics("main.cpp");
        await t.show("opened", at(await s.client.pullDiagnostics(s.uri("main.cpp"))));
        s.edit("main.cpp", EDIT);
        await t.show("edited", at(await s.client.pullDiagnostics(s.uri("main.cpp"))));
    });
}
