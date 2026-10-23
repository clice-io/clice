import { answers } from "@clice/tools/testing";

// operator== is used only in same()'s body, which a compile of main.cpp
// skips: the error -Werror makes of the warning that it is unused, which
// such a compile raises, is none in clice inspect nor in the editor.
answers(async (a) => {
    await a.show("equal", a.hover(a.marker("equal")));
});
