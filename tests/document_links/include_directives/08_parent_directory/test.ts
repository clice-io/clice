import { answers } from "@clice/tools/testing";

answers((a) => a.show("src/main.cpp", a.documentLinks("src/main.cpp")));
