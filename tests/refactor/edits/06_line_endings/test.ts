import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test(async (t) => {
    const crlf = (file: string): string => t.disk.read(file).replaceAll("\n", "\r\n");
    t.disk.write("lib.h", `\ufeff${crlf("lib.h")}`);
    t.disk.write("main.cpp", crlf("main.cpp"));
    await t.cli("index --jobs 2");
    await t.cli("refactor rename compute evaluate");
    expect(t.disk.read("lib.h")).toBe("\ufeff#pragma once\r\n\r\nint evaluate(int x);\r\n");
    expect(t.disk.read("main.cpp")).toBe(
        '#include "lib.h"\r\n\r\nint evaluate(int x) { return x; }\r\n\r\nint main() { return evaluate(1); }\r\n',
    );
});
