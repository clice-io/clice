import type { Diagnostic } from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// A compile's diagnostics are placed in the text it compiled. A push names
// that text's version: one sent after an edit, as a crash completing code
// republishes, still holds the positions of the text before it. A pull
// answers the buffer as it is now. The edit moves the error by characters
// that take a different number of units in each encoding.

const QUIET = { diagnostics: { clang_tidy: false }, project: { enable_indexing: false } };
const PULL = { textDocument: { diagnostic: {} } };
const ENCODINGS = [undefined, "utf-8", "utf-32"] as const;

/// Where the compiler's diagnostics start, as line:character.
function at(diagnostics: readonly Diagnostic[]): string[] {
    return diagnostics
        .filter(({ source }) => source === "clang")
        .map(({ range }) => `${String(range.start.line)}:${String(range.start.character)}`);
}

for (const positionEncoding of ENCODINGS) {
    const name = positionEncoding ?? "utf-16";

    test(`push ${name}`, async (t) => {
        const s = await t.start({
            config: QUIET,
            crash: { request: "completion", file: "main.cpp" },
            ...(positionEncoding ? { positionEncoding } : {}),
        });
        s.open("main.cpp");
        await s.pushes("main.cpp");
        s.edit("main.cpp", { replace: `"宽𝄞"`, with: `"宽𝄞宽𝄞宽𝄞"` });
        await s.completion(t.marker("complete"));
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
        const s = await t.start({
            config: QUIET,
            capabilities: PULL,
            crash: { request: "completion", file: "main.cpp" },
            ...(positionEncoding ? { positionEncoding } : {}),
        });
        await s.diagnostics("main.cpp");
        s.edit("main.cpp", { replace: `"宽𝄞"`, with: `"宽𝄞宽𝄞宽𝄞"` });
        await s.completion(t.marker("complete"));
        const pulled = await s.client.pullDiagnostics(s.uri("main.cpp"));
        await t.show("pulled", at(pulled));
    });
}
