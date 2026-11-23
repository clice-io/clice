import * as path from "node:path";
import { expect } from "vitest";
import * as proto from "vscode-languageserver-protocol";
import { test, type Serve, type TestContext } from "@clice/tools/testing";

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

/// Opens main.cpp under its upper-case URI and checks the URIs of what the
/// server answers and pushes: main.cpp's own, and other.h's under the folder.
async function answersInUpperCase(t: TestContext, s: Serve): Promise<void> {
    const decoded = (uri: string) => s.client.normalizeUri(uri);
    const main = upper(s.uri("main.cpp"));
    const other = upper(s.uri("other.h"));
    await s.client.sendNotification(proto.DidOpenTextDocumentNotification.type, {
        textDocument: { uri: main, languageId: "cpp", version: 0, text: t.disk.read("main.cpp") },
    });

    const { position } = s.position(t.at("main.cpp", "= |other()"));
    const definition = (await s.client.sendRequest(proto.DefinitionRequest.type, {
        textDocument: { uri: main },
        position,
    })) as proto.Location[];
    expect(definition.map(({ uri }) => decoded(uri))).toEqual([decoded(other)]);
    const references = await s.client.sendRequest(proto.ReferencesRequest.type, {
        textDocument: { uri: main },
        position,
        context: { includeDeclaration: true },
    });
    expect(new Set((references ?? []).map(({ uri }) => decoded(uri)))).toEqual(
        new Set([decoded(other), decoded(main)]),
    );

    // The requests compiled the document, which pushes its diagnostics.
    await s.sync();
    const pushes = s.client.publishedDiagnostics;
    expect(pushes.map(({ uri }) => decoded(uri))).toEqual([decoded(main)]);
    const actions = (await s.client.sendRequest(proto.CodeActionRequest.type, {
        textDocument: { uri: main },
        range: s.range(t.at("main.cpp", "other()")),
        context: { diagnostics: pushes[0]?.diagnostics ?? [] },
    })) as proto.CodeAction[];
    const edited = actions.flatMap(({ edit }) => [
        ...Object.keys(edit?.changes ?? {}),
        ...(edit?.documentChanges ?? []).flatMap((change) =>
            "textDocument" in change ? [change.textDocument.uri] : [],
        ),
    ]);
    expect(new Set(edited.map(decoded))).toEqual(new Set([decoded(main)]));
}

test("folders at initialize", WINDOWS, async (t) => {
    const s = await t.spawn();
    const root = upper(s.uri(""));
    await s.client.sendRequest(proto.InitializeRequest.type, {
        processId: process.pid,
        rootUri: root,
        workspaceFolders: [{ uri: root, name: "test" }],
        capabilities: {},
        initializationOptions: { cache_dir: path.join(t.root, ".clice") },
    });
    await s.client.sendNotification(proto.InitializedNotification.type, {});
    await answersInUpperCase(t, s);
});

test("a folder added", WINDOWS, async (t) => {
    const s = await t.start({ lastRule: { background_index: false } });
    const root = s.uri("");
    await s.client.sendNotification(proto.DidChangeWorkspaceFoldersNotification.type, {
        event: {
            removed: [{ uri: root, name: "test" }],
            added: [{ uri: upper(root), name: "test" }],
        },
    });
    await answersInUpperCase(t, s);
});
