import { answers } from "@clice/tools/testing";

answers("default", (a) => a.show("main.cpp", a.inlayHints("main.cpp")));
answers("names off", { config: { inlay_hints: { parameters: false } } }, (a) =>
    a.show("main.cpp", a.inlayHints("main.cpp")),
);
answers("markers off", { config: { inlay_hints: { mutable_references: false } } }, (a) =>
    a.show("main.cpp", a.inlayHints("main.cpp")),
);
