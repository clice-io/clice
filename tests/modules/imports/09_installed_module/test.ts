import { test } from "@clice/tools/testing";

// The module of a package installed outside the project, which CMake 4.5
// lists through the target it builds the module's BMI in: `--precompile`,
// with the module's options in a response file. The installed source
// compiles under that entry, finding the include directory it names, and
// provides the module to the project's importer.
test(async (t) => {
    const s = await t.start({ folders: ["app"] });
    await t.show("importer", s.errors("app/main.cpp"));
    await t.show("area", s.hover(t.at("app/main.cpp", "shapes::|area(")));
    await t.show("module", s.diagnostics("prefix/modules/shapes.cppm"));
    await t.show("command", s.compileCommand("prefix/modules/shapes.cppm"));
    await t.show("product", s.hover(t.at("prefix/modules/shapes.cppm", "detail::|product(")));
});
