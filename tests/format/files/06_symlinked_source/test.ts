import * as fs from "node:fs";
import * as path from "node:path";
import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The build names link.cpp, a link to real/main.cpp: the text is
// rewritten at the target, and the link stays a link.
test(async (t) => {
    t.disk.symlink("link.cpp", "real/main.cpp");
    await t.cli("format");
    expect(fs.lstatSync(path.join(t.root, "link.cpp")).isSymbolicLink()).toBe(true);
});
