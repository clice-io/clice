import { test } from "@clice/tools/testing";

test(async (t) => {
    const crlf = (file: string): string => t.disk.read(file).replaceAll("\n", "\r\n");
    t.disk.write("lib.h", `\ufeff${crlf("lib.h")}`);
    t.disk.write("main.cpp", crlf("main.cpp"));
    await t.cli("index --jobs 2");
    await t.cli("refactor rename compute evaluate");
});
