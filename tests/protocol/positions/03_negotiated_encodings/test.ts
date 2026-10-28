import { answers, type AnswersContext } from "@clice/tools/testing";

// A client that offers UTF-8 or UTF-32 positions gets them: every feature
// answers the same places in each encoding, on lines where characters
// outside ASCII, one of them outside the Basic Multilingual Plane, come
// before the name.

async function ask(a: AnswersContext): Promise<void> {
    await a.show("hover", a.hover(a.marker("call")));
    await a.show("include hover", a.hover(a.marker("link")));
    await a.show("completion", a.completion(a.marker("member")), { parts: ["edit"] });
    await a.show("signature help", a.signatureHelp(a.marker("argument")));
    await a.show("highlights", a.highlights(a.marker("sum")));
    await a.show("selection", a.selectionRange(a.marker("member")));
    await a.show("code actions", a.codeActions(a.marker("deduced"), { compile: true }));
    await a.show("tokens", a.semanticTokens("main.cpp"));
    await a.show("hints", a.inlayHints("main.cpp"));
    await a.show("hints in a range", a.inlayHints(a.at("main.cpp", "点{1, 2}")));
    await a.show("folds", a.foldingRanges("main.cpp"));
    await a.show("outline", a.documentSymbols("main.cpp"), { parts: ["range"] });
    await a.show("links", a.documentLinks("main.cpp"));
}

answers("utf-16", ask);
answers("utf-8", { positionEncoding: "utf-8" }, ask);
answers("utf-32", { positionEncoding: "utf-32" }, ask);

async function server(a: AnswersContext): Promise<void> {
    await a.show("navigation", {
        definition: a.definition(a.marker("call")),
        declaration: a.declaration(a.marker("call")),
        type_definition: a.typeDefinition(a.marker("parameter")),
        references: a.references(a.marker("field")),
    });
    await a.show("calls", {
        incoming_calls: a.incomingCalls(a.marker("call")),
        outgoing_calls: a.outgoingCalls(a.at("main.cpp", "int |用(")),
    });
    await a.show("加法", a.workspaceSymbols("加法"));
    await a.show("prepare rename", a.prepareRename(a.marker("field")));
    await a.show("rename", a.rename(a.marker("field"), "宽度"));
    await a.show("formatting", a.formatting(a.marker("format")));
    await a.show("include completion", a.completion(a.marker("header")), { parts: ["edit"] });
}

const SERVER = {
    server: "navigation, calls, rename and workspace symbols read the project index; inspect neither formats nor completes include paths",
};

answers("utf-16 server", { paths: SERVER }, server);
answers("utf-8 server", { paths: SERVER, positionEncoding: "utf-8" }, server);
answers("utf-32 server", { paths: SERVER, positionEncoding: "utf-32" }, server);
