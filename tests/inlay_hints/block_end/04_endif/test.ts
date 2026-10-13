import { answers } from "@clice/tools/testing";

answers("default", (a) => a.show("main.cpp", a.inlayHints("main.cpp")));
answers("disabled", { config: { inlay_hints: { endif: false } } }, (a) =>
    a.show("main.cpp", a.inlayHints("main.cpp")),
);
