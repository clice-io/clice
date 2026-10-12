import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Read-only serving: under "auto" and "on" an unedited open file is
// answered from the index, and builds stay pulled by the first request
// that needs one. "auto" turns a file to "off" at its first edit, or when
// the index cannot serve it. Files open without the diagnostics pull an
// editor of the push model never sends.
const AUTO = { config: { project: { readonly: "auto" } } };
const ON = { config: { project: { readonly: "on" } } };
const OFF = { config: { project: { readonly: "off" } } };
const EDITED = { after: "int main() { return twice(2); }\n", insert: "// edited\n" };

test("index serves unedited reads", async (t) => {
    const s = await t.start(AUTO);
    s.open("main.cpp", { pull: false });
    await s.sync();
    await t.show("tokens", s.semanticTokens("main.cpp"));
    await t.show("outline", s.documentSymbols("main.cpp"));
    await t.show("folds", s.foldingRanges("main.cpp"));
    await t.show("links", s.documentLinks("main.cpp"));
    await t.show("add", s.hover(t.marker("add")));
    await t.show("add definition", s.definition(t.marker("add")));
    await t.show("twice", s.hover(t.marker("twice")));
    // Without an AST, selection ranges come from the text alone.
    await t.show("x", {
        highlights: s.highlights(t.marker("x")),
        selection_ranges: s.selectionRange(t.marker("x")),
    });
    // Inlay hints need the AST, and nothing is compiled to push
    // diagnostics: both stay empty while the file is read-only.
    await t.show("hints", s.inlayHints("main.cpp"));
    await t.show("pushes", s.pushes("main.cpp"));
    expect((await s.counts()).pch).toBe(0);
});

// Outline and links have no refresh request: the reply waits for the
// file's index, which its opening moved to the front of the queue,
// instead of freezing an empty answer in the editor.
test("cold outline awaits the boost", async (t) => {
    const s = await t.start(AUTO);
    s.open("main.cpp", { pull: false });
    await t.show("cold", {
        outline: s.documentSymbols("main.cpp"),
        links: s.documentLinks("main.cpp"),
    });
});

// The edit turns the file to "off"; the build itself waits for the next
// read, whose compile pushes the diagnostics.
test("edit escalates to compile", async (t) => {
    const s = await t.start(AUTO);
    s.open("main.cpp", { pull: false });
    await s.sync();
    s.edit("main.cpp", EDITED);
    await t.show("edited", s.pushes("main.cpp"));
    await t.show("read", s.hover(t.at("main.cpp", "return |add(x, x)")));
    await t.show("pushed", s.pushes("main.cpp"));
});

// A restored unsaved buffer differs from what the index holds: the open
// itself escalates, so the first read pulls a compile instead of answering
// empty from rows that no longer fit the text.
test("diverged open buffer escalates", async (t) => {
    const s = await t.start(AUTO);
    s.open("main.cpp", { pull: false });
    await s.indexed();
    await s.stop();
    await s.start();
    s.open("main.cpp", {
        text: `${t.disk.read("main.cpp")}// restored, unsaved\n`,
        pull: false,
    });
    await t.show("restored", s.documentSymbols("main.cpp"));
    await t.show("pushed", s.pushes("main.cpp"));
});

test("readonly on builds no pch", async (t) => {
    const s = await t.start(ON);
    s.open("main.cpp", { pull: false });
    await s.sync();
    // Completion still answers, from a full parse without a preamble.
    await t.show("completion", s.completion(t.at("main.cpp", "return twic|e(2)")));
    await t.show("pushes", s.pushes("main.cpp"));
    expect((await s.counts()).pch).toBe(0);
});

test("readonly on pull builds nothing", async (t) => {
    const s = await t.start({ ...ON, capabilities: { textDocument: { diagnostic: {} } } });
    s.open("main.cpp", { pull: false });
    await s.sync();
    await t.show("pulled", s.diagnostics("main.cpp"));
    expect((await s.counts()).pch).toBe(0);
});

// The edit turns the file to "off": the inlay hints asked again ride the
// pulled compile and answer from the AST.
test("escalation upgrades inlay hints", async (t) => {
    const s = await t.start(AUTO);
    s.open("main.cpp", { pull: false });
    await s.sync();
    await t.show("read-only", s.inlayHints("main.cpp"));
    s.edit("main.cpp", EDITED);
    await t.show("edited", s.inlayHints("main.cpp"));
});

// Under "on" a diverged buffer cannot escalate: answers from the index
// withdraw rather than map rows of the old text onto the new.
test("diverged buffer serves no links", async (t) => {
    const s = await t.start(AUTO);
    s.open("main.cpp", { pull: false });
    await s.indexed();
    await s.stop();
    const on = await t.start(ON);
    on.open("main.cpp", {
        text: t.disk.read("main.cpp").replace('#include "header.h"', '#include "renamed.h"'),
        pull: false,
    });
    await t.show("diverged", {
        links: on.documentLinks("main.cpp"),
        definition: on.definition(t.at("main.cpp", '#include "|renamed.h"')),
    });
});

// Under "off" opening a file starts nothing: the first read pulls the
// compile, and its diagnostics follow. No index runs, so the read is
// answered by that compile.
test("off compiles on demand", async (t) => {
    const s = await t.start({
        config: { project: { readonly: "off", enable_indexing: false } },
    });
    s.open("main.cpp", { pull: false });
    await t.show("opened", s.pushes("main.cpp"));
    await t.show("read", s.hover(t.marker("add")));
    await t.show("pushed", s.pushes("main.cpp"));
});

// Under "off" the first read pulls the compile but does not wait for it:
// the index of an earlier session answers while the compile is held.
test("index answers while pull compile runs", async (t) => {
    const s = await t.start(AUTO);
    s.open("main.cpp", { pull: false });
    await s.indexed();
    await s.stop();
    const off = await t.start(OFF);
    const compile = await off.park("compile", "main.cpp", { at: "worker" });
    off.open("main.cpp", { pull: false });
    await t.show("outline", off.documentSymbols("main.cpp"));
    await compile.reached();
    await compile.release();
    await t.show("pushed", off.pushes("main.cpp"));
});

// Completion is an edit intent: under "auto" it turns the file to "off",
// so the next read pulls a compile, answers from it, and its diagnostics
// are pushed.
test("completion escalates", async (t) => {
    const s = await t.start(AUTO);
    s.open("main.cpp", { pull: false });
    await s.sync();
    await t.show("completion", s.completion(t.at("main.cpp", "return twic|e(2)")));
    await t.show("completed", s.pushes("main.cpp"));
    await t.show("read", s.hover(t.marker("add")));
    await t.show("pushed", s.pushes("main.cpp"));
});

// A header has no command of its own: read-only, it is answered from the
// rows its includer's index holds.
test("header served read-only", async (t) => {
    const s = await t.start(AUTO);
    s.open("header.h", { pull: false });
    await s.sync();
    await t.show("header", {
        outline: s.documentSymbols("header.h"),
        hover: s.hover(t.at("header.h", "int |add(")),
        references: s.references(t.at("header.h", "int |add(")),
    });
    await t.show("pushed", s.pushes("header.h"));
    expect((await s.counts()).pch).toBe(0);
});
