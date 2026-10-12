import { answers } from "@clice/tools/testing";

answers((a) => a.show("main.cpp", a.documentLinks("main.cpp")));
