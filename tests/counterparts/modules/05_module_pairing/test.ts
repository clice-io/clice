import { answers } from "@clice/tools/testing";

// A module pairing is the weakest evidence: two units implementing colors
// leave the choice to the user; shapes.cppm wins over shapes.h, both of the
// same name, by the module alone; and compute.cpp, declared by its
// interface and by the partition it imports as much, ranks the interface
// first but does not take it directly.
answers(
    { paths: { server: "counterparts read the build and the project index" }, indexing: true },
    async (a) => {
        for (const file of ["colors.cppm", "shapes.cpp", "compute.cpp"]) {
            await a.show(file, a.counterparts(file));
        }
    },
);
