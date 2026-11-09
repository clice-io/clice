import { test } from "@clice/tools/testing";

test({ errors: ["Toolchain query failed: Cannot find driver: cl-missing\\.exe"] }, async (t) => {
    const s = await t.start();
    for (const file of ["greeting.cppm", "detail.cpp", "util.ixx", "main.cpp"]) {
        await t.show(file, s.errors(file));
    }
    await t.show("greet", s.hover(t.at("main.cpp", "return twice(|greet())")));
    await t.show("twice", s.hover(t.at("main.cpp", "return |twice(")));
});
