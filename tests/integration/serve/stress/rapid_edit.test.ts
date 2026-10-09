/// Integration tests for rapid editing: ensure no hang and correct hover results.

import { at, expect, serve } from "../../fixtures.ts";

/// 50 rapid edits, each followed by a hover on 'add' function.
///
/// The file has #include <iostream> so PCH build is non-trivial.
/// This must not hang and the final hover must return correct results.
serve.data("hello_world")("rapid edits with hover", async ({ s }) => {
    await s.compiled("main.cpp");
    const add = at("main.cpp", "int |add(");
    expect(await s.hover(add), "Initial hover on 'add' should not be None").not.toBeNull();

    // 50 rapid body edits, each followed by a hover request.
    const content = s.disk.read("main.cpp");
    for (let i = 0; i < 50; i++) {
        s.edit("main.cpp", { text: content.replace("return a + b;", `return a + b + ${i};`) });
        // Fire-and-forget hover on 'add' — just ensure it doesn't hang.
        // We don't await the result here to simulate real editor behavior
        // where requests overlap.
        void s.hover(add).catch(() => undefined);
    }
    await s.sync();

    // Final hover must succeed and return correct result.
    const finalHover = await s.hover(add);
    expect(finalHover, "Final hover returned None — worker may have hung").not.toBeNull();
    expect(finalHover!.contents, "Final hover contents should not be None").not.toBeNull();

    s.close("main.cpp");
});
