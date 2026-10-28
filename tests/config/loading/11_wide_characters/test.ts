import { test } from "@clice/tools/testing";

// A warning's place on a line where characters outside ASCII, one of them
// outside the Basic Multilingual Plane, come before it, in each encoding
// the client counts positions in.
for (const encoding of ["utf-16", "utf-8", "utf-32"] as const) {
    test(encoding, async (t) => {
        const s = await t.start(encoding === "utf-16" ? {} : { positionEncoding: encoding });
        await t.show("clice.toml", s.pushes("clice.toml"));
    });
}
