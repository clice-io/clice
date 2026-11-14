import { answers } from "@clice/tools/testing";

answers("default", (a) => a.show("main.cpp", a.inlayHints("main.cpp")));
answers("configured", { config: { inlay_hints: { max_length: { types: "unlimited" } } } }, (a) =>
    a.show("main.cpp", a.inlayHints("main.cpp")),
);
