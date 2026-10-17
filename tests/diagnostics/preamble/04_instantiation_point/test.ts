import { test } from "@clice/tools/testing";

// The header instantiates lib::show<app::Point>, whose call to describe is
// found by ADL only at a point that sees main.cpp's overload. The
// precompiled preamble instantiates it where the preamble ends, before that
// overload: ill-formed, no diagnostic required, and clang compiling the file
// as a whole instantiates it at the end and accepts it.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["related"] });
});
