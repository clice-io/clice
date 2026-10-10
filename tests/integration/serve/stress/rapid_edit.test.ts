/// Integration tests for rapid editing: ensure no hang and correct hover results.

import { at, expect, serve } from "../../fixtures.ts";

/// 50 rapid edits, each followed by a hover on the 'report' function.
///
/// The file includes the standard library so the PCH build is non-trivial.
/// This must not hang and the final hover must return correct results.
serve("stdlib")("rapid edits with hover", async ({ s }) => {
    await s.compiled("report.cpp");
    const report = at("report.cpp", "std::string |report(");
    expect(await s.hover(report), "Initial hover on 'report' should not be None").not.toBeNull();

    // 50 rapid body edits, each followed by a hover request.
    const content = s.disk.read("report.cpp");
    for (let i = 0; i < 50; i++) {
        s.edit("report.cpp", {
            text: content.replace("counts[name] += 1;", `counts[name] += ${i + 1};`),
        });
        // Fire-and-forget hover on 'report' — just ensure it doesn't hang.
        // We don't await the result here to simulate real editor behavior
        // where requests overlap.
        void s.hover(report).catch(() => undefined);
    }
    await s.sync();

    // Final hover must succeed and return correct result.
    const finalHover = await s.hover(report);
    expect(finalHover, "Final hover returned None — worker may have hung").not.toBeNull();
    expect(finalHover!.contents, "Final hover contents should not be None").not.toBeNull();

    s.close("report.cpp");
});
