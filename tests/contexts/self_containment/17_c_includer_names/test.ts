import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Each header leans on what its includer defines first in a way C compiles
// alone without a missing-name error: a struct named in a prototype (a
// warning), an enum as a field's type, a struct as an array's elements, a
// macro after a declarator, a macro declaring at file scope, a list of
// enumerators two braces deep in an initializer; C++ reads names.h's list
// as declarations without a type.
test(async (t) => {
    const s = await t.start();
    const headers = ["proto.h", "fields.h", "entries.h", "api.h", "docs.h", "kinds.h", "names.h"];
    for (const header of headers) {
        await t.show(header, s.diagnostics(header));
    }
    expect((await s.stats()).synthesizedContexts).toBe(headers.length);
});
