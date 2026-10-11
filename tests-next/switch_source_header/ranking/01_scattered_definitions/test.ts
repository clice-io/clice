import { answers } from "@clice/tools/testing";

answers(
    { paths: { server: "counterparts read the build and the project index" }, indexing: true },
    async (a) => {
        for (const file of ["text.h", "text_case.cpp"]) {
            await a.show(file, a.counterparts(file));
        }
    },
);
