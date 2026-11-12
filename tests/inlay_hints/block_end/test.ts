import { answers } from "@clice/tools/testing";

// Block ends alone, after blocks of any height.
answers(
    {
        config: {
            inlay_hints: {
                block_end: true,
                min_block_lines: 2,
                parameters: false,
                deduced_types: false,
                designators: false,
            },
        },
    },
    (a) => a.show("main.cpp", a.inlayHints("main.cpp")),
);
