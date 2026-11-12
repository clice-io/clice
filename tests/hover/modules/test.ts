import { answers } from "@clice/tools/testing";

answers(
    {
        paths: {
            server: "the card comes from the index of module interfaces, which clice inspect does not build",
        },
    },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.hover(m));
        }
    },
);
