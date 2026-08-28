/// Integration tests for rapid editing: ensure no hang and correct hover results.

import { SETTLE_TIME, sleep, withTimeout } from "@clice/tools/client";
import { cliceTest, expect } from "../fixtures.ts";

const test = cliceTest("hello_world");
const EDIT_INTERVAL = 20;

/// 50 rapid edits, each followed by a hover on 'add' function.
///
/// The file has #include <iostream> so PCH build is non-trivial.
/// This must not hang and the final hover must return correct results.
test("rapid edits with hover", async ({ client }) => {
    const [uri, content] = await client.openAndWait("main.cpp");

    // Hover on 'add' (line 2, char 4) to verify initial state.
    const hover = await withTimeout(client.hoverAt(uri, 2, 4), 30_000, "hover");
    expect(hover, "Initial hover on 'add' should not be None").not.toBeNull();

    // 50 rapid body edits, each followed by a hover request.
    for (let i = 0; i < 50; i++) {
        const newContent = content.replace("return a + b;", `return a + b + ${i};`);
        client.change(uri, i + 2, newContent);
        // Fire-and-forget hover on 'add' — just ensure it doesn't hang.
        // We don't await the result here to simulate real editor behavior
        // where requests overlap.
        void client.hoverAt(uri, 2, 4).catch(() => undefined);
        await sleep(EDIT_INTERVAL);
    }

    // Wait a moment for in-flight requests to settle.
    await sleep(SETTLE_TIME * 2);

    // Final hover must succeed and return correct result.
    const finalHover = await withTimeout(client.hoverAt(uri, 2, 4), 30_000, "hover");
    expect(finalHover, "Final hover returned None — worker may have hung").not.toBeNull();
    expect(finalHover!.contents, "Final hover contents should not be None").not.toBeNull();

    client.close(uri);
});
