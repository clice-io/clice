import { expect } from "vitest";
import { answers, test } from "@clice/tools/testing";

// CMake gives every unit of a target with precompiled headers the target's
// PCH and forces its header in; cmake_pch.hxx.cxx builds that PCH. app's
// commands take the shape CMake writes for clang (`-Xclang -include-pch`,
// `-Xclang -emit-pch`), tool's the one it writes for GCC (the `.gch` beside
// the forced header), under clang++ so that every platform runs them.
// Nothing is built on disk: clice builds each PCH itself from its builder's
// command. main.cpp and tool.cpp use what the header holds without
// including it, area.cpp includes it too. The header warns where it is
// parsed: in the PCH build, not in the units loading the PCH.

const app = "build/CMakeFiles/app.dir/cmake_pch.hxx.cxx";
const tool = "build/CMakeFiles/tool.dir/cmake_pch.hxx.cxx";

answers(
    "navigation",
    {
        paths: { server: "definition reads the server's preambles" },
        // Only the PCHs can answer: nothing is indexed.
        config: { project: { enable_indexing: false } },
    },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.definition(m));
        }
    },
);

test("units share the target's pch", async (t) => {
    const s = await t.start();
    for (const file of ["src/main.cpp", "src/area.cpp", "src/tool.cpp"]) {
        await t.show(file, s.diagnostics(file));
    }
    await s.indexed();
    // One build of each target's PCH serves the open files and the index:
    // area.cpp's preamble PCH is built over app's, main.cpp's and
    // tool.cpp's parses load theirs. Those three, nothing else.
    const { files } = await s.counts();
    expect({
        app: files[app]?.pch,
        tool: files[tool]?.pch,
        main: files["src/main.cpp"]?.pch,
        area: files["src/area.cpp"]?.pch,
        built: (await s.stats()).pchCacheEntries,
    }).toEqual({ app: 1, tool: 1, main: 0, area: 1, built: 3 });
    await t.show("area", s.references(t.at("include/shapes.h", "inline double |area(")));
    await t.show("completion", s.completion(t.at("src/main.cpp", "return are|a(")));
});

test("the targets index", async (t) => {
    await t.cli("index");
});

test("a header of the pch changes", async (t) => {
    const s = await t.start();
    expect(await s.errors("src/main.cpp")).toEqual([]);
    await s.indexed();
    t.disk.write(
        "include/shapes.h",
        t.disk.read("include/shapes.h").replaceAll("area(", "circle_area("),
    );
    await t.show("main.cpp", s.errors("src/main.cpp"));
    await s.indexed();
    // The PCH is built again for the new header, and the closed unit over
    // it indexes again.
    const { files } = await s.counts();
    expect({ app: files[app]?.pch, area: files["src/area.cpp"]?.index }).toEqual({
        app: 2,
        area: 2,
    });
    await t.show("circle_area", s.workspaceSymbols("circle_area"));
});

test("a restart keeps the pch a preamble is built over", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.diagnostics("src/area.cpp")).toEqual([]);
    await s.restart();
    expect(await s.diagnostics("src/area.cpp")).toEqual([]);
    await s.sync();
    expect((await s.counts()).pch).toBe(0);
    await t.show("chained", s.definition(t.marker("chained")));
});
