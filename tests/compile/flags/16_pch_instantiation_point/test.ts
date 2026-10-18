import { test } from "@clice/tools/testing";

// The target's PCH instantiates apply<Early> where it ends, as CMake has
// clang build it (-fpch-instantiate-templates), before main.cpp declares
// helper(Early): its call reaches helper(...). Parsed whole, main.cpp
// instantiates it at its end, where the call reaches helper(Early). Two
// points of instantiation giving apply<Early> different meanings make the
// program ill-formed, no diagnostic required.

test("instantiation point", async (t) => {
    await t.cli("index");
    await t.cli(`query references ${t.place(t.marker("early"))} --include-declaration`);
});
