import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Run from the case's root on the app workspace: the cache goes where
// ${workspace} puts it, the logs under app rather than the directory the
// command ran in, and nothing to the default app/.clice. Both are named
// .clice, which a command's written files leave out.
test(async (t) => {
    await t.cli("index --workspace app");
    expect(t.disk.list("app/build/.clice/**").length > 0).toBe(true);
    expect(t.disk.list("app/.clice/**")).toEqual([]);
});
