import { answers } from "@clice/tools/testing";

answers({ config: { inlay_hints: { array_indices: true } } }, (a) =>
    a.show("main.cpp", a.inlayHints("main.cpp")),
);
