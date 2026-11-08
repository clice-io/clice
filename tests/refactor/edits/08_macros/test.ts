import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --jobs 2");
    // A file changed since it was indexed that spells neither name.
    t.disk.edit("other.cpp", { after: "return 1; }\n", insert: "int more() { return 2; }\n" });
    // The definitions of one file are one macro: its branches, and its
    // redefinition after an #undef.
    await t.cli("refactor rename LIMIT CAPACITY");
});
