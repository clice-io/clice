import { test } from "@clice/tools/testing";

// clang++ compiles each unit clean under its own command.
test(async (t) => {
    const s = await t.start();
    await t.show("host.cpp", s.diagnostics("host.cpp"));
    await t.show("device.cpp", s.diagnostics("device.cpp"));
    await t.show("kernel.cu", s.diagnostics("kernel.cu"));
});
