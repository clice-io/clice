import { answers, type AnswersContext } from "@clice/tools/testing";

// A client that offers UTF-8 positions gets them: every feature answers the
// same places in either encoding, on lines where characters outside ASCII,
// one of them outside the Basic Multilingual Plane, come before the name.

async function ask(a: AnswersContext): Promise<void> {
    await a.show("hover", a.hover(a.marker("call")));
    await a.show("completion", a.completion(a.marker("member")), { parts: ["edit"] });
    await a.show("signature help", a.signatureHelp(a.marker("argument")));
    await a.show("highlights", a.highlights(a.marker("sum")));
    await a.show("selection", a.selectionRange(a.marker("member")));
    await a.show("code actions", a.codeActions(a.marker("deduced"), { compile: true }));
    await a.show("tokens", a.semanticTokens("main.cpp"));
    await a.show("hints", a.inlayHints("main.cpp"));
    await a.show("folds", a.foldingRanges("main.cpp"));
    await a.show("outline", a.documentSymbols("main.cpp"), { parts: ["range"] });
    await a.show("links", a.documentLinks("main.cpp"));
}

answers("utf-16", ask);
answers("utf-8", { positionEncoding: "utf-8" }, ask);

async function index(a: AnswersContext): Promise<void> {
    await a.show("navigation", {
        definition: a.definition(a.marker("call")),
        declaration: a.declaration(a.marker("call")),
        type_definition: a.typeDefinition(a.marker("parameter")),
        references: a.references(a.marker("field")),
    });
    await a.show("加法", a.workspaceSymbols("加法"));
    await a.show("prepare rename", a.prepareRename(a.marker("field")));
    await a.show("rename", a.rename(a.marker("field"), "宽度"));
}

const SERVER = { server: "navigation, rename and workspace symbols read the project index" };

answers("utf-16 index", { paths: SERVER }, index);
answers("utf-8 index", { paths: SERVER, positionEncoding: "utf-8" }, index);
