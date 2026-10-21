import { answers } from "@clice/tools/testing";

answers(async (a) => {
    await a.show("include", a.hover(a.at("main.cpp", '#include |"工具.h"')));
});
