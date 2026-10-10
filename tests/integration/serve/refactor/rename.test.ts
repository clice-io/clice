/// The editor's prepareRename/rename answer from the persisted index, with
/// the engine `clice refactor rename` plans by.

import * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import { applyTextEdits } from "@clice/tools/client/edits";
import { canonicalUri } from "@clice/tools/workspace";
import { at, expect, serve } from "../../fixtures.ts";

const HEADER = [
    "#pragma once",
    "int compute(int x);",
    "struct Widget { Widget(); int value; };",
    "#define CALL compute(1)",
    "",
].join("\n");
const MAIN = [
    '#include "a.h"',
    "int compute(int x) { return x; }",
    "Widget::Widget() : value(0) {}",
    "int use() { return compute(2) + CALL; }",
    "int main() { Widget w; return use() + w.value; }",
    "",
].join("\n");
const OTHER = ['#include "a.h"', "int again() { return compute(3); }", ""].join("\n");

const project = serve.files({
    "clice.toml": '[project]\ncache_dir = "${workspace}/.clice"\n',
    "a.h": HEADER,
    "main.cpp": MAIN,
    "other.cpp": OTHER,
});

/// The edits a rename reply makes to `file`, and the buffer version they
/// were computed for.
function changeOf(edit: proto.WorkspaceEdit | null, s: Serve, file: string) {
    for (const change of edit?.documentChanges ?? []) {
        if ("textDocument" in change && canonicalUri(change.textDocument.uri) === s.uri(file)) {
            const edits = change.edits.filter((item): item is proto.TextEdit => "newText" in item);
            return { version: change.textDocument.version, edits };
        }
    }
    return null;
}

const COMPUTE = at("main.cpp", "int co|mpute(int x) {");

function rename(s: Serve, newName: string) {
    return s.request<proto.WorkspaceEdit | null>("textDocument/rename", COMPUTE, { newName });
}

project("renames from the editor", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.indexed();

    const prepared = await s.request("textDocument/prepareRename", COMPUTE);
    expect(prepared).toEqual({
        range: { start: { line: 1, character: 4 }, end: { line: 1, character: 11 } },
        placeholder: "compute",
    });

    const notices: string[] = [];
    s.client.onNotification(proto.ShowMessageNotification.type, (params) => {
        notices.push(params.message);
    });
    const edit = await rename(s, "evaluate");
    const main = changeOf(edit, s, "main.cpp");
    expect(main?.version).toBe(0);
    expect(applyTextEdits(MAIN, main!.edits)).toBe(
        MAIN.replace("int compute", "int evaluate").replace("compute(2)", "evaluate(2)"),
    );
    const header = changeOf(edit, s, "a.h");
    expect(header?.version).toBeNull();
    expect(applyTextEdits(HEADER, header!.edits)).toBe(
        HEADER.replace("int compute", "int evaluate"),
    );
    const other = changeOf(edit, s, "other.cpp");
    expect(applyTextEdits(OTHER, other!.edits)).toBe(OTHER.replace("compute", "evaluate"));
    // The notice goes out with the reply or as a send the server
    // scheduled, which sync waits for.
    await s.sync();
    expect(notices.join("\n")).toContain("#define CALL compute(1)");
});

project("the editor hears why not", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.indexed();

    await expect(
        s.request("textDocument/prepareRename", at("main.cpp", "compute(2) + C|ALL")),
    ).rejects.toThrow("macro");
    await expect(rename(s, "use")).resolves.not.toBeNull();
    await expect(rename(s, "Widget")).rejects.toThrow("already declared in the same scope");
    expect(await s.request("textDocument/prepareRename", at("main.cpp", "#include"))).toBeNull();
});

serve.files(
    { "main.cpp": "int compute();\nint use() { return compute(); }\n" },
    { databases: false, launch: { folders: [] } },
)("a rootless server refuses up front", async ({ s }) => {
    await s.compiled("main.cpp");

    const loc = at("main.cpp", "int c|ompute");
    await expect(s.request("textDocument/prepareRename", loc)).rejects.toThrow("workspace folder");
    await expect(s.request("textDocument/rename", loc, { newName: "evaluate" })).rejects.toThrow(
        "workspace folder",
    );
});
