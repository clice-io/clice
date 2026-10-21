import { test } from "@clice/tools/testing";

// A thousand nested instantiations take clang about 4.5 MB of stack, more
// than the 1 MB main thread lld gives a Windows program by default: clang's
// own guard moves to a fresh stack only as its usage nears 8 MB.
test(async (t) => {
    await t.cli(["inspect", "folding-range", "main.cpp"]);
});
