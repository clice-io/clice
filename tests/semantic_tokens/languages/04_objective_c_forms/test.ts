import { answers } from "@clice/tools/testing";

answers((a) => a.show("main.m", a.semanticTokens("main.m")));
