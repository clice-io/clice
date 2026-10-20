import { answers } from "@clice/tools/testing";

// stdbool.h's `bool` reaches main.c only through the preamble, and main.c
// never spells it: the macro stays unread until the hover asks.
answers(async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.hover(m));
    }
});
