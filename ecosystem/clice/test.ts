import { regression } from "@clice/tools/ecosystem";

regression({
    known: [
        {
            issue: "11-04#1",
            why: "a module unit whose global module fragment includes <string_view> reads clice::version as another std::string_view than the std module's std::string takes",
            kind: "compile",
            detail: /no matching conversion for functional-style cast from 'const std::string_view'/,
        },
        {
            issue: "11-04#1",
            why: "the same, where clice::version is formatted: the std module's formatter takes no std::string_view the fragment's <string_view> declares",
            kind: "compile",
            detail: /no member named 'parse' in 'std::(?:__1::)?formatter<std::(?:__1::)?string_view>'/,
        },
    ],
});
