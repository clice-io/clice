import { answers } from "@clice/tools/testing";

answers((a) => a.show("main.hip", a.semanticTokens("main.hip")));
