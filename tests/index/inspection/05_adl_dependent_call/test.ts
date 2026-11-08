import { test } from "@clice/tools/testing";

// widget.h calls to_text on a member of a dependent pair, a function
// argument-dependent lookup finds in text.h alone, and asks for the size of
// what it returns. a.cpp includes text.h before widget.h, b.cpp does not;
// widget.h does not include it itself and means the same in both units:
// one variant, with neither the call nor size resolved. c.cpp, a focus
// file, sees its whole unit and keeps both.
test(async (t) => {
    await t.cli("index -j 1");
    await t.cli("index file widget.h");
    await t.cli("index file c.cpp");
});
