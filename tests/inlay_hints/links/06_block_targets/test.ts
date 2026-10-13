import { answers } from "@clice/tools/testing";

answers({ config: { inlay_hints: { block_end: true, min_block_lines: 2 } } }, (a) =>
    a.show("main.cpp", a.inlayHints("main.cpp"), { parts: ["links"] }),
);
