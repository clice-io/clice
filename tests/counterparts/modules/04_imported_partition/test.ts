import { answers } from "@clice/tools/testing";

// detail.cpp is an internal partition holding declarations alone, which
// shapes.cpp imports and defines: written in a source, it still declares
// for its importer, and the two switch to each other as a header and its
// source do.
answers(
    { paths: { server: "counterparts read the build and the project index" }, indexing: true },
    async (a) => {
        for (const file of ["detail.cpp", "shapes.cpp"]) {
            await a.show(file, a.counterparts(file));
        }
    },
);
