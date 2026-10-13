import { answers } from "@clice/tools/testing";

// Only an interface unit exports.
answers("interface", (a) => a.show("interface", a.completion(a.marker("interface"))));

answers(
    "implementation",
    {
        paths: {
            server: "clice inspect compiles one file alone, where an implementation unit without its interface recovers as one",
        },
    },
    (a) => a.show("implementation", a.completion(a.marker("implementation"))),
);
