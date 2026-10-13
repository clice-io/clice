import { answers } from "@clice/tools/testing";

answers("default", (a) => a.show("main.cpp", a.inlayHints("main.cpp")));
answers("configured", { config: { inlay_hints: { prefixed_names: true } } }, (a) =>
    a.show("main.cpp", a.inlayHints("main.cpp")),
);
