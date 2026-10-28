import { answers } from "@clice/tools/testing";

answers((a) => a.show("main.cpp", a.foldingRanges("main.cpp")));
