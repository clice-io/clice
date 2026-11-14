import { test } from "@clice/tools/testing";

// macOS writes `._name` beside a file on volumes without extended
// attributes (and archives carry them): binary metadata that keeps the
// file's suffix, which no walk of the sources takes for one.
test(async (t) => {
    t.disk.write("src/._main.cpp", "\u0000\u0005\u0016\u0007\u0000\u0002\u0000\u0000Mac OS X");
    await t.cli("index");
});
