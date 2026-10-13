import { test } from "@clice/tools/testing";

// Asked again as typing in the call asks, the popup showing another overload
// than the one ranked first.
test(async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    const second = t.marker("second");
    await t.show("asked", s.signatureHelp(second));
    await t.show("picked", s.signatureHelp(second, "draw(double x, double y) -> void"));
    await t.show("not listed", s.signatureHelp(second, "draw() -> void"));
});
