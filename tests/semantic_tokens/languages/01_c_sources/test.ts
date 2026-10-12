import { answers } from "@clice/tools/testing";

answers((a) => a.show("main.c", a.semanticTokens("main.c")));
