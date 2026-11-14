import { test } from "@clice/tools/testing";

// A client without pull support, saving a header while the file including
// it compiles: the compile running finishes, and one more follows with the
// last text, not one per save; only the last is pushed. An open file the
// header does not reach compiles nothing.
test(async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    s.open("other.cpp");
    s.open("shapes.h");
    await t.show("opened", s.pushes("main.cpp"));
    const park = await s.park("compile", "main.cpp", { at: "worker" });
    s.edit("shapes.h", { replace: "int area();", with: "int width();" });
    s.save("shapes.h");
    await park.reached();
    s.edit("shapes.h", { replace: "int width();", with: "int height();" });
    s.save("shapes.h");
    s.edit("shapes.h", { replace: "int height();", with: "int area();" });
    s.save("shapes.h");
    await park.release();
    await t.show("saved", s.pushes("main.cpp"));
    // The background index's passes are its own; the compiles and the PCHs
    // are what the saves cost.
    const counts = await s.counts();
    const cost = (file: string) => {
        const built = counts.files[file];
        return { compile: built?.compile ?? 0, pch: built?.pch ?? 0, publish: built?.publish ?? 0 };
    };
    await t.show("builds", { main: cost("main.cpp"), other: cost("other.cpp") });
});
