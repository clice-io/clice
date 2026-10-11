import { answers } from "@clice/tools/testing";

answers("default", (a) => a.show("main.cpp", a.inlayHints("main.cpp")));
answers("configured", { config: { inlay_hints: { type_name_limit: 0 } } }, (a) =>
    a.show("main.cpp", a.inlayHints("main.cpp")),
);
