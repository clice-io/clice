import { answers } from "@clice/tools/testing";

// The source defining the most declarations is taken directly only with at
// least twice as many as the next: 3 against 2 leaves the choice to the
// user, 2 against 1 does not. Equal evidence orders by directory distance,
// then by path.
answers(
    { paths: { server: "counterparts read the build and the project index" }, indexing: true },
    async (a) => {
        for (const file of ["palette.h", "compass.h", "units.h"]) {
            await a.show(file, a.counterparts(file));
        }
    },
);
