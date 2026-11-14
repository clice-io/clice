import { answers } from "@clice/tools/testing";

// The length limit lifted: the whole spelling is what the case shows.
answers({ config: { inlay_hints: { max_length: { types: "unlimited" } } } }, (a) =>
    a.show("main.cpp", a.inlayHints("main.cpp")),
);
