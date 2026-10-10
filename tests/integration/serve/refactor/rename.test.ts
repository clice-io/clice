/// The editor's prepareRename/rename answer from the persisted index, with
/// the engine `clice refactor rename` plans by.

import * as proto from "vscode-languageserver-protocol";
import type { Loc, Serve } from "@clice/tools/actions";
import { applyTextEdits } from "@clice/tools/client/edits";
import { canonicalUri } from "@clice/tools/workspace";
import { at, expect, serve } from "../../fixtures.ts";

const project = serve("shapes/headers");

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

function rename(s: Serve, loc: Loc, newName: string) {
    return s.request<proto.WorkspaceEdit | null>("textDocument/rename", loc, { newName });
}

project("renames from the editor", async ({ s }) => {
    await s.compiled(s.file("circle_impl"));
    await s.indexed();

    const area = at(s.file("circle_impl"), "double ar|ea(const Circle& circle) {");
    const prepared = await s.request("textDocument/prepareRename", area);
    const { position } = s.position(at(s.file("circle_impl"), "double |area(const Circle&"));
    expect(prepared).toEqual({
        range: {
            start: position,
            end: { ...position, character: position.character + "area".length },
        },
        placeholder: "area",
    });

    const notices: string[] = [];
    s.client.onNotification(proto.ShowMessageNotification.type, (params) => {
        notices.push(params.message);
    });
    const edit = await rename(s, area, "surface");
    const renamed = (file: string) =>
        applyTextEdits(s.disk.read(file), changeOf(edit, s, file)!.edits);
    expect(
        (edit?.documentChanges ?? [])
            .map((change) => ("textDocument" in change ? s.relative(change.textDocument.uri) : ""))
            .sort(),
    ).toEqual([s.file("circle"), s.file("circle_impl"), s.file("main"), s.file("test")].sort());
    expect(changeOf(edit, s, s.file("circle_impl"))?.version).toBe(0);
    expect(renamed(s.file("circle_impl"))).toBe(
        s.disk
            .read(s.file("circle_impl"))
            .replace("return area(", "return surface(")
            .replace("double area(", "double surface("),
    );
    expect(changeOf(edit, s, s.file("circle"))?.version).toBeNull();
    expect(renamed(s.file("circle"))).toBe(
        s.disk.read(s.file("circle")).replace("double area(", "double surface("),
    );
    for (const caller of [s.file("main"), s.file("test")]) {
        expect(renamed(caller)).toBe(
            s.disk.read(caller).replace("shapes::area(c)", "shapes::surface(c)"),
        );
    }
    // The notice goes out with the reply or as a send the server
    // scheduled, which sync waits for.
    await s.sync();
    expect(notices.join("\n")).toContain("#define SHAPES_AREA(circle) shapes::area(circle)");
});

project("the editor hears why not", async ({ s }) => {
    await s.compiled(s.file("circle_impl"));
    await s.indexed();

    await expect(
        s.request("textDocument/prepareRename", at(s.file("circle"), "= SHAPES_|PI;")),
    ).rejects.toThrow("macro");
    const area = at(s.file("circle_impl"), "double ar|ea(const Circle& circle) {");
    await expect(rename(s, area, "registry_count")).resolves.not.toBeNull();
    await expect(rename(s, area, "Circle")).rejects.toThrow("already declared in the same scope");
    expect(
        await s.request(
            "textDocument/prepareRename",
            at(s.file("circle_impl"), '#include "shapes/circle.h"'),
        ),
    ).toBeNull();
});

serve("tiny", { databases: false, launch: { folders: [] } })(
    "a rootless server refuses up front",
    async ({ s }) => {
        await s.compiled("main.cpp");

        const loc = at("main.cpp", "int a|dd(");
        await expect(s.request("textDocument/prepareRename", loc)).rejects.toThrow(
            "workspace folder",
        );
        await expect(
            s.request("textDocument/rename", loc, { newName: "evaluate" }),
        ).rejects.toThrow("workspace folder");
    },
);
