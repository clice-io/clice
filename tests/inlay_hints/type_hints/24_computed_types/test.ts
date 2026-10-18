import { answers } from "@clice/tools/testing";

// The length limit lifted: the whole spelling is what the case shows.
answers({ config: { inlay_hints: { max_length: { types: 0 } } } }, (a) =>
    a.show("main.cpp", a.inlayHints("main.cpp")),
);
