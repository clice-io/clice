import * as path from "node:path";
import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules file util/clock.h");
    await t.cli("analyze modules file app/main.cpp");
    // An absolute path names the same file.
    await t.cli(["analyze", "modules", "file", path.join(t.root, "app", "main.cpp")]);
});
