import { answers } from "@clice/tools/testing";

answers((a) => a.show("outline", a.documentSymbols("main.cpp"), { parts: ["detail", "tags"] }));
