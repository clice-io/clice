/// Integration tests for the clice MasterServer.

import { execFileSync } from "node:child_process";
import * as proto from "vscode-languageserver-protocol";
import { sleep, SETTLE_TIME, withTimeout } from "@clice/tools/client";
import { cliceExecutable, cliceTest, expect } from "../fixtures.ts";

const test = cliceTest("hello_world");
const EDIT_INTERVAL = 50;

function capabilityEnabled(capability: unknown): boolean {
    return capability !== undefined && capability !== null && capability !== false;
}

test("server info", ({ client }) => {
    expect(client.initResult!.serverInfo!.name).toBe("clice");
    // The version is injected at build time (git describe or the base
    // version); instead of pinning a value, pin that the LSP handshake and
    // the --version CLI report the same thing.
    const stdout = execFileSync(cliceExecutable(), ["--version"], {
        encoding: "utf8",
        timeout: 10_000,
    });
    const cliVersion = stdout.trim().replace(/^clice version /, "");
    expect(cliVersion.length).toBeGreaterThan(0);
    expect(/[0-9]/.test(cliVersion[0]!)).toBe(true);
    expect(client.initResult!.serverInfo!.version).toBe(cliVersion);
});

test("capabilities", ({ client }) => {
    const caps = client.initResult!.capabilities;
    expect(caps.hoverProvider).toBe(true);
    expect(caps.completionProvider).toBeDefined();
    expect(caps.completionProvider!.triggerCharacters).toContain(" ");
    expect(capabilityEnabled(caps.definitionProvider)).toBe(true);
    expect(capabilityEnabled(caps.documentSymbolProvider)).toBe(true);
    expect(capabilityEnabled(caps.foldingRangeProvider)).toBe(true);
    expect(capabilityEnabled(caps.inlayHintProvider)).toBe(true);
    // codeAction is not implemented yet, so it must not be advertised.
    expect(capabilityEnabled(caps.codeActionProvider)).toBe(false);
    // workspace/didChangeWorkspaceFolders is not handled, so workspace
    // folder support must not be advertised.
    expect(
        caps.workspace === undefined || !capabilityEnabled(caps.workspace.workspaceFolders),
    ).toBe(true);
    expect(caps.documentFormattingProvider).toBe(true);
    expect(caps.documentRangeFormattingProvider).toBe(true);
    expect(caps.semanticTokensProvider).toBeDefined();
});

test("semantic token modifier legend", ({ client }) => {
    const legend = (
        client.initResult!.capabilities.semanticTokensProvider as proto.SemanticTokensOptions
    ).legend;
    expect(legend).toBeDefined();
    expect(legend.tokenTypes).toContain("identifier");
    expect([...legend.tokenModifiers]).toEqual([
        "declaration",
        "definition",
        "const",
        "overloaded",
        "typed",
        "templated",
        "deprecated",
        "deduced",
        "readonly",
        "static",
        "abstract",
        "virtual",
        "dependentName",
        "defaultLibrary",
        "usedAsMutableReference",
        "usedAsMutablePointer",
        "constructorOrDestructor",
        "userDefined",
        "functionScope",
        "classScope",
        "fileScope",
        "globalScope",
        "inactive",
    ]);
});

test("did open close cycle", async ({ client }) => {
    const [uri] = client.open("main.cpp");
    await sleep(SETTLE_TIME);
    client.close(uri);
});

test("shutdown exit", async ({ client }) => {
    await client.sendRequest(proto.ShutdownRequest.type);
});

test("feature requests after close", async ({ client }) => {
    const [uri] = client.open("main.cpp");
    client.close(uri);
    await expect(client.hoverAt(uri, 0, 0)).rejects.toThrow("Document not open");
});

test("incremental change", async ({ client }) => {
    const [uri, initial] = client.open("main.cpp");
    let content = initial;
    for (let i = 0; i < 5; i++) {
        content += `\n// change ${i}`;
        client.change(uri, i + 1, content);
        await sleep(EDIT_INTERVAL);
    }
    await sleep(SETTLE_TIME * 2);
    client.close(uri);
});

test("diagnostics received", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    expect(client.diagnostics.has(uri)).toBe(true);
    client.close(uri);
});

test("close clears diagnostics", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    const arrived = client.armDiagnostics(uri);
    client.close(uri);
    await withTimeout(arrived, 10_000, `diagnostics ${uri}`);
    expect(client.diagnostics.get(uri)).toEqual([]);
});

test("hover before compile", async ({ client }) => {
    const [uri] = client.open("main.cpp");
    await client.hoverAt(uri, 0, 0);
    client.close(uri);
}, 90_000);

test("completion request", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    await client.completionAt(uri, 0, 0);
    client.close(uri);
});

test("signature help request", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    await client.signatureHelpAt(uri, 0, 0);
    client.close(uri);
});

test("definition request", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    await client.definitionAt(uri, 2, 4);
    client.close(uri);
});

test("document symbol request", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    const result = await client.documentSymbols(uri);
    expect(result).not.toBeNull();
    client.close(uri);
});

test("folding range request", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    const result = await client.foldingRanges(uri);
    expect(result).not.toBeNull();
    client.close(uri);
});

test("semantic tokens request", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    const result = await client.semanticTokensFull(uri);
    expect(result).not.toBeNull();
    client.close(uri);
});

test("inlay hint request", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    await client.inlayHints(uri, {
        start: { line: 0, character: 0 },
        end: { line: 10, character: 0 },
    });
    client.close(uri);
});

test("code action request", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    await client.codeActions(uri, {
        start: { line: 0, character: 0 },
        end: { line: 0, character: 10 },
    });
    client.close(uri);
});

test("document link request", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    const result = await client.documentLinks(uri);
    expect(result).not.toBeNull();
    client.close(uri);
});

test("rapid changes stress", async ({ client }) => {
    const [uri, initial] = client.open("main.cpp");
    let content = initial;
    for (let i = 0; i < 20; i++) {
        content += `\n// stress change ${i}\n`;
        client.change(uri, i + 1, content);
    }
    await sleep(SETTLE_TIME * 4);
    client.close(uri);
});

test("save notification", async ({ client }) => {
    const [uri] = client.open("main.cpp");
    await sleep(SETTLE_TIME);
    client.save(uri);
    await sleep(SETTLE_TIME);
    client.close(uri);
});

test("hover on unknown file", async ({ client }) => {
    await expect(client.hoverAt("file:///nonexistent/fake.cpp", 0, 0)).rejects.toThrow(
        "Document not open",
    );
});

test("hover out of range position", async ({ client }) => {
    // Positions beyond the document clamp to the end of content (LSP spec).
    const [uri] = await client.openAndWait("main.cpp");
    await client.hoverAt(uri, 99999, 0);
});

test("format range out of range", async ({ client }) => {
    // Range endpoints beyond the document clamp as well (forward_format path).
    const [uri] = await client.openAndWait("main.cpp");
    await client.formatRange(uri, {
        start: { line: 0, character: 999 },
        end: { line: 9999, character: 0 },
    });
});

/// Exercise all feature requests after compilation completes.
test("all features after compile wait", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");

    const hover = await client.hoverAt(uri, 2, 4);
    expect(hover).not.toBeNull();

    await client.completionAt(uri, 7, 18);

    await client.signatureHelpAt(uri, 0, 0);

    await client.definitionAt(uri, 2, 4);

    const symbols = await client.documentSymbols(uri);
    expect(symbols).not.toBeNull();

    const folding = await client.foldingRanges(uri);
    expect(folding).not.toBeNull();

    const tokens = await client.semanticTokensFull(uri);
    expect(tokens).not.toBeNull();

    const links = await client.documentLinks(uri);
    expect(links).not.toBeNull();

    await client.codeActions(uri, {
        start: { line: 0, character: 0 },
        end: { line: 0, character: 10 },
    });

    await client.inlayHints(uri, {
        start: { line: 0, character: 0 },
        end: { line: 10, character: 0 },
    });

    client.close(uri);
});
