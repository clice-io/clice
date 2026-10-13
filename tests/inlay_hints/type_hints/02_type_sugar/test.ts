import { answers } from "@clice/tools/testing";

answers("default", (a) => a.show("main.cpp", a.inlayHints("main.cpp")));
answers("configured", { config: { inlay_hints: { max_length: { types: 0 } } } }, (a) =>
    a.show("main.cpp", a.inlayHints("main.cpp")),
);
