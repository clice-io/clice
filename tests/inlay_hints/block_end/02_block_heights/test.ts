import { answers } from "@clice/tools/testing";

answers({ config: { inlay_hints: { block_end: true } } }, (a) =>
    a.show("main.cpp", a.inlayHints("main.cpp")),
);
