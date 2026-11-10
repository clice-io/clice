import { answers } from "@clice/tools/testing";

answers((a) => a.show("main.mm", a.semanticTokens("main.mm")));
