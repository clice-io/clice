import { test } from "@clice/tools/testing";

// The database is Latin-1, as a build writes a legacy -D value: its é is no
// UTF-8. Only a.cpp's entry holds it; b.cpp's is plain ASCII.
test(async (t) => {
    await t.cli("index");
});
