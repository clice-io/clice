import { test } from "@clice/tools/testing";

// Asked again as typing in the call asks, the popup showing another overload
// than the one ranked first.
test(async (t) => {
    const s = await t.start();
    s.open("main.cpp");
    const second = t.marker("second");
    const asked = await s.signatureHelp(second);
    await t.show("asked", s.signatureHelp(second));
    const labels = asked.signatures.map((signature) => signature.label);
    await t.show("picked", s.signatureHelp(second, { signatures: labels, active: 2 }));
    await t.show(
        "not listed",
        s.signatureHelp(second, { signatures: ["draw() -> void", ...labels], active: 0 }),
    );
});
