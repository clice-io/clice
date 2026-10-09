/// Integration tests for the clice MasterServer.

import * as proto from "vscode-languageserver-protocol";
import { runProcess } from "@clice/tools/client";
import { at, cliceExecutable, expect, serve } from "../../fixtures.ts";

const test = serve.data("hello_world");

const ADD = at("main.cpp", "add(int a");
const FIRST_LINE = at("main.cpp", "#include <iostream>");
const FIRST_TEN = { start: { line: 0, character: 0 }, end: { line: 0, character: 10 } };
const LINES_0_TO_10 = { start: { line: 0, character: 0 }, end: { line: 10, character: 0 } };

function capabilityEnabled(capability: unknown): boolean {
    return capability !== undefined && capability !== null && capability !== false;
}

test("server info", async ({ s }) => {
    const info = s.client.initResult!.serverInfo!;
    expect(info.name).toBe("clice");
    // The version is injected at build time (git describe or the base
    // version); instead of pinning a value, pin that the LSP handshake and
    // the --version CLI report the same thing.
    const { status, stdout, stderr } = await runProcess(cliceExecutable(), ["--version"], {
        timeout: 10_000,
    });
    expect(status, stderr).toBe(0);
    const cliVersion = stdout.trim().replace(/^clice version /, "");
    expect(cliVersion.length).toBeGreaterThan(0);
    expect(/[0-9]/.test(cliVersion[0]!)).toBe(true);
    expect(info.version).toBe(cliVersion);
});

test("capabilities", ({ s }) => {
    const caps = s.client.initResult!.capabilities;
    expect(caps.hoverProvider).toBe(true);
    expect(caps.completionProvider).toBeDefined();
    expect(caps.completionProvider!.triggerCharacters).toContain(" ");
    expect(capabilityEnabled(caps.definitionProvider)).toBe(true);
    expect(capabilityEnabled(caps.documentSymbolProvider)).toBe(true);
    expect(capabilityEnabled(caps.foldingRangeProvider)).toBe(true);
    expect(capabilityEnabled(caps.inlayHintProvider)).toBe(true);
    expect(capabilityEnabled(caps.codeActionProvider)).toBe(true);
    // Every workspace folder is a project, and folders come and go.
    expect(caps.workspace?.workspaceFolders?.supported).toBe(true);
    expect(caps.workspace?.workspaceFolders?.changeNotifications).toBe(true);
    expect(caps.documentFormattingProvider).toBe(true);
    expect(caps.documentRangeFormattingProvider).toBe(true);
    expect(caps.semanticTokensProvider).toBeDefined();
    return Promise.resolve();
});

test("semantic token modifier legend", ({ s }) => {
    const legend = (
        s.client.initResult!.capabilities.semanticTokensProvider as proto.SemanticTokensOptions
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
    return Promise.resolve();
});

test("did open close cycle", async ({ s }) => {
    s.open("main.cpp");
    await s.sync();
    s.close("main.cpp");
    await s.sync();
});

test("shutdown exit", async ({ s }) => {
    await s.stop();
});

test("feature requests after close", async ({ s }) => {
    s.open("main.cpp");
    s.close("main.cpp");
    await expect(s.hover(FIRST_LINE)).rejects.toThrow("Document not open");
});

test("incremental change", async ({ s }) => {
    s.open("main.cpp");
    let content = s.disk.read("main.cpp");
    for (let i = 0; i < 5; i++) {
        content += `\n// change ${i}`;
        s.edit("main.cpp", { text: content });
    }
    await s.sync();
    s.close("main.cpp");
    await s.sync();
});

test("diagnostics received", async ({ s }) => {
    await s.compiled("main.cpp");
    expect((await s.counts()).files["main.cpp"]?.publish).toBeGreaterThan(0);
});

test("close clears diagnostics", async ({ s }) => {
    await s.compiled("main.cpp");
    const before = (await s.counts()).files["main.cpp"]?.publish ?? 0;
    s.close("main.cpp");
    await s.sync();
    expect(s.client.publishCount(s.uri("main.cpp"))).toBe(before + 1);
    expect(s.client.lastPublish(s.uri("main.cpp"))?.diagnostics).toEqual([]);
});

test("hover before compile", async ({ s }) => {
    s.open("main.cpp");
    await s.hover(FIRST_LINE);
    s.close("main.cpp");
});

test("completion request", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.request("textDocument/completion", FIRST_LINE);
});

test("signature help request", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.request("textDocument/signatureHelp", FIRST_LINE);
});

test("definition request", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.definition(ADD);
});

test("document symbol request", async ({ s }) => {
    await s.compiled("main.cpp");
    expect(await s.request("textDocument/documentSymbol", "main.cpp")).not.toBeNull();
});

test("folding range request", async ({ s }) => {
    await s.compiled("main.cpp");
    expect(await s.request("textDocument/foldingRange", "main.cpp")).not.toBeNull();
});

test("semantic tokens request", async ({ s }) => {
    await s.compiled("main.cpp");
    expect(await s.request("textDocument/semanticTokens/full", "main.cpp")).not.toBeNull();
});

test("inlay hint request", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.request("textDocument/inlayHint", "main.cpp", { range: LINES_0_TO_10 });
});

test("code action request", async ({ s }) => {
    await s.compiled("main.cpp");
    await s.request("textDocument/codeAction", "main.cpp", {
        range: FIRST_TEN,
        context: { diagnostics: [] },
    });
});

test("document link request", async ({ s }) => {
    await s.compiled("main.cpp");
    expect(await s.request("textDocument/documentLink", "main.cpp")).not.toBeNull();
});

test("rapid changes stress", async ({ s }) => {
    s.open("main.cpp");
    let content = s.disk.read("main.cpp");
    for (let i = 0; i < 20; i++) {
        content += `\n// stress change ${i}\n`;
        s.edit("main.cpp", { text: content });
    }
    await s.sync();
    s.close("main.cpp");
    await s.sync();
});

test("save notification", async ({ s }) => {
    s.open("main.cpp");
    await s.sync();
    s.save("main.cpp");
    await s.sync();
    s.close("main.cpp");
    await s.sync();
});

test("hover on unknown file", async ({ s }) => {
    await expect(s.client.hoverAt("file:///nonexistent/fake.cpp", 0, 0)).rejects.toThrow(
        "Document not open",
    );
});

test("hover out of range position", async ({ s }) => {
    // Positions beyond the document clamp to the end of content (LSP spec).
    await s.compiled("main.cpp");
    await s.request("textDocument/hover", "main.cpp", { position: { line: 99999, character: 0 } });
});

test("format range out of range", async ({ s }) => {
    // Range endpoints beyond the document clamp as well (forward_format path).
    await s.compiled("main.cpp");
    await s.request("textDocument/rangeFormatting", "main.cpp", {
        range: { start: { line: 0, character: 999 }, end: { line: 9999, character: 0 } },
        options: { tabSize: 4, insertSpaces: true },
    });
});

/// Exercise all feature requests after compilation completes.
test("all features after compile wait", async ({ s }) => {
    await s.compiled("main.cpp");

    expect(await s.hover(ADD)).not.toBeNull();
    await s.request("textDocument/completion", at("main.cpp", '"|hello world"'));
    await s.request("textDocument/signatureHelp", FIRST_LINE);
    await s.definition(ADD);
    expect(await s.request("textDocument/documentSymbol", "main.cpp")).not.toBeNull();
    expect(await s.request("textDocument/foldingRange", "main.cpp")).not.toBeNull();
    expect(await s.request("textDocument/semanticTokens/full", "main.cpp")).not.toBeNull();
    expect(await s.request("textDocument/documentLink", "main.cpp")).not.toBeNull();
    await s.request("textDocument/codeAction", "main.cpp", {
        range: FIRST_TEN,
        context: { diagnostics: [] },
    });
    await s.request("textDocument/inlayHint", "main.cpp", { range: LINES_0_TO_10 });
});
