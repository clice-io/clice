import { answers } from "@clice/tools/testing";

answers({ config: { document_symbols: { macros: false } } }, (a) =>
    a.show("outline", a.documentSymbols("main.cpp"), { parts: ["detail"] }),
);
