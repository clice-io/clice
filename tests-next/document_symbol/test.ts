import { answers } from "@clice/tools/testing";

answers((a) => a.show("main.cpp", a.documentSymbols("main.cpp")));
