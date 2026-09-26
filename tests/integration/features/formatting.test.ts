import * as fs from "node:fs";
import * as proto from "vscode-languageserver-protocol";
import { test, expect } from "../fixtures.ts";

const UNFORMATTED = "int    add(   int   a  ,  int   b  ) {\nreturn   a+b ;\n}\n";
const FORMATTED = "int add(int a, int b) { return a + b; }\n";

/// Apply LSP TextEdits to a string, processing from end to start.
function applyEdits(text: string, edits: proto.TextEdit[]): string {
    let lines = text.split("\n");
    const sorted = [...edits].sort((a, b) =>
        b.range.start.line !== a.range.start.line
            ? b.range.start.line - a.range.start.line
            : b.range.start.character - a.range.start.character,
    );
    for (const edit of sorted) {
        const start = edit.range.start;
        const end = edit.range.end;
        const before =
            lines.slice(0, start.line).join("\n") +
            (start.line > 0 ? "\n" : "") +
            lines[start.line]!.slice(0, start.character);
        const after =
            lines[end.line]!.slice(end.character) +
            (end.line < lines.length - 1 ? "\n" : "") +
            lines.slice(end.line + 1).join("\n");
        text = before + edit.newText + after;
        lines = text.split("\n");
    }
    return text;
}

test("format document", async ({ session }) => {
    const { client } = await session("formatting");
    const [uri] = await client.openAndWait("main.cpp");

    client.change(uri, 1, UNFORMATTED);
    const edits = await client.formatDocument(uri);

    expect(edits).not.toBeNull();
    expect(edits!.length).toBeGreaterThan(0);
    const result = applyEdits(UNFORMATTED, edits!);
    expect(result).toBe(FORMATTED);

    client.close(uri);
});

test("format range", async ({ session }) => {
    const { client } = await session("formatting");
    const [uri] = await client.openAndWait("main.cpp");

    client.change(uri, 1, UNFORMATTED);
    const edits = await client.formatRange(uri, {
        start: { line: 1, character: 0 },
        end: { line: 2, character: 0 },
    });

    expect(edits).not.toBeNull();
    expect(edits!.length).toBeGreaterThan(0);

    client.close(uri);
});

test("format already formatted", async ({ session }) => {
    const { client } = await session("formatting");
    const [uri] = await client.openAndWait("main.cpp");

    client.change(uri, 1, FORMATTED);
    const edits = await client.formatDocument(uri);

    expect(edits).not.toBeNull();
    expect(edits!.length).toBe(0);

    client.close(uri);
});

test.skipIf(process.platform === "win32")(
    "style found beside the opened name",
    async ({ session }) => {
        const { client, workspace } = session.tmp();
        workspace.write("third_party/lib.cpp", UNFORMATTED);
        workspace.write("third_party/.clang-format", "BasedOnStyle: LLVM\n");
        workspace.write(
            "src/.clang-format",
            "BasedOnStyle: LLVM\nIndentWidth: 8\nAllowShortFunctionsOnASingleLine: None\n",
        );
        fs.symlinkSync(workspace.path("third_party/lib.cpp"), workspace.path("src/lib.cpp"));
        workspace.writeCDB(["src/lib.cpp"]);
        await client.initialize(workspace);

        const [uri] = await client.openAndWait("src/lib.cpp");
        const edits = await client.formatDocument(uri);
        expect(applyEdits(UNFORMATTED, edits ?? [])).toBe(
            "int add(int a, int b) {\n        return a + b;\n}\n",
        );
    },
);
