import { answers } from "@clice/tools/testing";

// FAIL_VALUE's expansion is the last source location entry of the server's
// preamble PCH, and clang's SourceManager takes a PCH's last entry to run
// on past its end: the default argument's range ends nowhere and prints by
// its value. The fix belongs in clang.
answers(
    {
        paths: {
            separate:
                "the server's preamble PCH ends with the default argument's expansion, whose end clang cannot find; inspect builds no preamble",
        },
    },
    async (a) => {
        await a.show("member", a.completion(a.marker("member")));
        await a.show("call", a.hover(a.marker("call")));
    },
);
