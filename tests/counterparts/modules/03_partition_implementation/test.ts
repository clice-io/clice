import { answers } from "@clice/tools/testing";

// circle.cpp is an internal partition that defines what the primary
// interface declares: the two switch to each other as an implementation
// unit and its interface do.
answers(
    { paths: { server: "counterparts read the build and the project index" }, indexing: true },
    async (a) => {
        for (const file of ["circle.cppm", "circle.cpp"]) {
            await a.show(file, a.counterparts(file));
        }
    },
);
