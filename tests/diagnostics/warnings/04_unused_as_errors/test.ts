import { answers } from "@clice/tools/testing";

// operator== is used only in same()'s body: no warning says it is unused,
// nor the error -Werror would make of one, in clice inspect or in the editor.
answers(async (a) => {
    await a.show("equal", a.hover(a.marker("equal")));
});
