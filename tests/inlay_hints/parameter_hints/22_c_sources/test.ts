import { answers } from "@clice/tools/testing";

answers((a) => a.show("main.c", a.inlayHints("main.c")));
