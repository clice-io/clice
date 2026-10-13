import { answers } from "@clice/tools/testing";

answers(
    {
        config: {
            inlay_hints: {
                block_end: true,
                default_arguments: true,
                min_block_lines: 2,
                max_length: { block_end: 12, endif: 16, default_arguments: 18 },
            },
        },
    },
    (a) => a.show("main.cpp", a.inlayHints("main.cpp")),
);
