import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

// CMake gives every unit of a target with precompiled headers the target's
// PCH (`-Xclang -include-pch`) and forces its header in; cmake_pch.hxx.cxx
// builds that PCH (`-Xclang -emit-pch`). Nothing is built on disk: clice
// builds the PCH itself from the builder's command. main.cpp uses what the
// header holds without including it, area.cpp includes it too.

const builder = "build/CMakeFiles/app.dir/cmake_pch.hxx.cxx";

answers(
    "navigation",
    { paths: { server: "definition reads the server's preambles" } },
    async (a) => {
        await a.show("use", a.definition(a.marker("use")));
    },
);

test("units share the target's pch", async (t) => {
    const s = await t.start();
    expect(await s.errors("src/main.cpp")).toEqual([]);
    expect(await s.errors("src/area.cpp")).toEqual([]);
    await s.indexed();
    // One build of the target's PCH serves the open files and the index:
    // area.cpp's preamble PCH is built over it, main.cpp's parse loads it.
    // Both built, nothing else.
    const { files } = await s.counts();
    expect({
        builder: files[builder]?.pch,
        main: files["src/main.cpp"]?.pch,
        area: files["src/area.cpp"]?.pch,
        built: (await s.stats()).pchCacheEntries,
    }).toEqual({ builder: 1, main: 0, area: 1, built: 2 });
    await t.show("area", s.references(t.at("include/shapes.h", "inline double |area(")));
});

test("the target indexes", async (t) => {
    await t.cli("index");
});

test("a header of the pch changes", async (t) => {
    const s = await t.start();
    expect(await s.errors("src/main.cpp")).toEqual([]);
    await s.indexed();
    t.disk.edit("include/shapes.h", {
        replace: "inline double area(",
        with: "inline double surface(",
    });
    await t.show("main.cpp", s.errors("src/main.cpp"));
    await s.indexed();
    // The PCH is built again for the new header, and the closed unit over
    // it indexes again.
    const { files } = await s.counts();
    expect({ builder: files[builder]?.pch, area: files["src/area.cpp"]?.index }).toEqual({
        builder: 2,
        area: 2,
    });
    await t.show("surface", s.workspaceSymbols("surface"));
});
