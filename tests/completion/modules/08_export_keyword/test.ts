import { answers } from "@clice/tools/testing";

// Only an interface unit exports, not an implementation unit or partition.
answers("interface", (a) => a.show("interface", a.completion(a.marker("interface"))));

answers(
    "implementation",
    {
        paths: {
            server: "clice inspect compiles one file alone, where an implementation unit without its interface recovers as one",
        },
    },
    async (a) => {
        await a.show("implementation", a.completion(a.marker("implementation")));
        await a.show(
            "partition_implementation",
            a.completion(a.marker("partition_implementation")),
        );
    },
);
