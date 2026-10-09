/// The action layer against a real server: each action ends on the event it
/// names.

import { at, expect, serve } from "../fixtures.ts";

serve("tiny")("buffer edits reach the compile", async ({ s }) => {
    await s.clean("main.cpp");
    s.edit("main.cpp", { replace: "add(1, 2)", with: "add(1)" });
    const errors = await s.errors("main.cpp");
    expect(s.show(errors.map(({ range }) => ({ uri: s.uri("main.cpp"), range })))).toBe(
        "main.cpp: int value = add(1);",
    );
    expect(s.show(await s.hover(at("main.cpp", "int a|dd(")))).toContain(
        "int add(int lhs, int rhs)",
    );
    expect(s.disk.read("main.cpp")).toContain("add(1, 2)");

    s.save("main.cpp");
    expect(s.disk.read("main.cpp")).toContain("add(1);");
    s.close("main.cpp");
    expect((await s.counts()).files["main.cpp"]?.publish).toBe(3);
});

serve("tiny")("restart keeps the index, cold rebuilds it", async ({ s }) => {
    await s.indexed();
    expect((await s.counts()).index).toBe(1);
    await s.restart();
    await s.indexed();
    expect((await s.counts()).index).toBe(0);
    await s.cold();
    await s.indexed();
    expect((await s.counts()).index).toBe(1);
});

serve.files({ "main.cpp": "int main() {\n    auto value = 1;\n    return value;\n}\n" })(
    "code action edits the buffer",
    async ({ s }) => {
        await s.clean("main.cpp");
        const { text, diagnostics } = await s.applyAction(
            at("main.cpp", "auto value"),
            "Replace 'auto' with 'int'",
        );
        expect(text).toContain("    int value = 1;");
        expect(diagnostics).toEqual([]);
        expect(s.disk.read("main.cpp")).toContain("auto value");
    },
);
