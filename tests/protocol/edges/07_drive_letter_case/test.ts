import * as proto from "vscode-languageserver-protocol";
import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A client writing the drive letter in upper case gets every URI back the
// way it writes them: Qt Creator keys a document's version by its path,
// case and all, and shows no diagnostics pushed under another case.

const WINDOWS = {
    except: {
        linux: "only a Windows path has a drive letter",
        darwin: "only a Windows path has a drive letter",
    },
};

/// `uri` as Qt Creator writes it: the drive letter in upper case, its colon
/// not encoded.
function upper(uri: string): string {
    return uri.replace(
        /^file:\/\/\/([a-z])(?::|%3A)/i,
        (_, drive: string) => `file:///${drive.toUpperCase()}:`,
    );
}

/// The URIs of `locations`, decoded: the server may encode a character
/// otherwise than the harness, but never changes the drive's case.
function uris(locations: proto.Location[] | null): string[] {
    return (locations ?? []).map((location) => decodeURIComponent(location.uri));
}

test("a drive in upper case", WINDOWS, async (t) => {
    const s = await t.start({ lastRule: { background_index: false } });
    const root = s.uri("");
    await s.client.sendNotification(proto.DidChangeWorkspaceFoldersNotification.type, {
        event: {
            removed: [{ uri: root, name: "test" }],
            added: [{ uri: upper(root), name: "test" }],
        },
    });
    const main = upper(s.uri("main.cpp"));
    const other = decodeURIComponent(upper(s.uri("other.h")));
    const text = t.disk.read("main.cpp");
    await s.client.sendNotification(proto.DidOpenTextDocumentNotification.type, {
        textDocument: { uri: main, languageId: "cpp", version: 0, text },
    });
    const lines = text.split("\n");
    const line = lines.findIndex((each) => each.includes("other()"));
    const position = { line, character: (lines[line] ?? "").indexOf("other()") };
    const definition = await s.client.sendRequest(proto.DefinitionRequest.type, {
        textDocument: { uri: main },
        position,
    });
    expect(uris(definition as proto.Location[])).toEqual([other]);
    const references = await s.client.sendRequest(proto.ReferencesRequest.type, {
        textDocument: { uri: main },
        position,
        context: { includeDeclaration: true },
    });
    expect(new Set(uris(references))).toEqual(new Set([other, decodeURIComponent(main)]));
    // The requests compiled the document, which pushes its diagnostics.
    await s.sync();
    const pushed = s.client.publishedDiagnostics.map((params) => params.uri);
    expect(pushed).toEqual([main]);
});
