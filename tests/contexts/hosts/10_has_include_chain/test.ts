import { answers } from "@clice/tools/testing";

// The scan reads both branches of main.cpp's `__has_include`; its compile
// takes the one through fast.h, and so does detail.h's context on both
// clients.
answers(async (a) => {
    await a.show("value", a.hover(a.marker("value")));
});
