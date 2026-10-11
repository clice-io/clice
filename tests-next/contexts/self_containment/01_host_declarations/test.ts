import { answers } from "@clice/tools/testing";

answers(
    {
        paths: {
            server: "clice inspect compiles a header with its host's command but without the host's declarations before the include",
        },
    },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.hover(m));
        }
    },
);
