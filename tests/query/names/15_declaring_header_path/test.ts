import { test } from "@clice/tools/testing";

// Widget::draw and free_function are declared in a.h and defined in a.cpp;
// Widget::hide is defined in a.h. `path:` names the files a symbol is
// declared in, so path:a.h finds all four.
test(async (t) => {
    await t.cli("index");
    await t.cli(["query", "symbol-search", "draw path:a.h"]);
    await t.cli(["query", "symbol-search", "path:a.h"]);
});
