import { answers } from "@clice/tools/testing";

answers(
    { paths: { server: "counterparts read the build and the project index" }, indexing: true },
    async (a) => {
        for (const file of ["shapes.cppm", "shapes_debug.cpp"]) {
            await a.show(file, a.counterparts(file));
        }
    },
);
