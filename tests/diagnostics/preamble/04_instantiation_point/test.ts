import { test } from "@clice/tools/testing";

// The header instantiates lib::show<app::Point>, whose call to describe is
// found by argument-dependent lookup only at the end of the file, in
// main.cpp. The precompiled preamble, which instantiates what its headers
// use where it ends, fails to and is built again without instantiating:
// no error clang compiling the whole file does not raise, and main.cpp keeps
// the warnings an error in the preamble would mute. demo() deduces its
// return type, so the preamble parses its body, as it skips other header
// functions'.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["related"] });
});
