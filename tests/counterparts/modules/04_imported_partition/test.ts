import { answers, test } from "@clice/tools/testing";

// detail.cpp is an internal partition holding declarations alone, which
// shapes.cpp imports and defines: written in a source, it still declares
// for its importer and switches to it, as a header to its source, and
// shapes.cpp lists it after its interface. edges.cppm, a partition no unit
// imports, defines what the interface declares: written in a module
// interface file, it still pairs with the interface as an implementation
// unit does.
answers(
    "editor",
    { paths: { server: "counterparts read the build and the project index" }, indexing: true },
    async (a) => {
        for (const file of ["detail.cpp", "edges.cppm", "shapes.cpp"]) {
            await a.show(file, a.counterparts(file));
        }
    },
);

// The command line reads which partitions are imported from the index it
// built.
test("command line", async (t) => {
    await t.cli("index");
    await t.cli("query counterparts detail.cpp");
    await t.cli("query counterparts edges.cppm");
});
