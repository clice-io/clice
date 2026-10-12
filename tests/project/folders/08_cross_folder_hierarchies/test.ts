import { test } from "@clice/tools/testing";

test("hierarchies cross folders", async (t) => {
    const s = await t.start({ folders: ["app", "lib"] });
    await s.indexed();
    // The library's function is called from the application only, and its
    // interface implemented there only.
    s.open("lib/src/lib.cpp");
    await s.sync();
    await t.show("callers", s.incomingCalls(t.at("lib/src/lib.cpp", "int |lib_fn")));
    s.open("lib/include/shape.h");
    await s.sync();
    const shape = t.at("lib/include/shape.h", "struct |Shape");
    await t.show("subtypes", s.subtypes(shape));
    await t.show("implementations", s.implementation(shape));
    // The other way round the library knows nothing of the application's
    // type, yet its base stays reachable though the library serves the
    // base's open file.
    s.open("app/main.cpp");
    await s.sync();
    await t.show("supertypes", s.supertypes(t.at("app/main.cpp", "struct |Square")));
});
