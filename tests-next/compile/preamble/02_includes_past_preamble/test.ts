import { answers } from "@clice/tools/testing";

// The preamble ends at the first declaration: it holds only a #define and
// records no link, so every include here is one of the body's.
answers(async (a) => {
    await a.show("links", a.documentLinks("main.cpp"));
    await a.show("late", a.hover(a.marker("late")));
    // A call named include is no directive.
    await a.show("call", a.hover(a.marker("call")));
});
