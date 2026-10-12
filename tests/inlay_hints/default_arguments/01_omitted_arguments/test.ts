import { answers } from "@clice/tools/testing";

answers("default", (a) => a.show("main.cpp", a.inlayHints("main.cpp"), { parts: ["links"] }));
answers("configured", { config: { inlay_hints: { default_arguments: true } } }, (a) =>
    a.show("main.cpp", a.inlayHints("main.cpp"), { parts: ["links"] }),
);
