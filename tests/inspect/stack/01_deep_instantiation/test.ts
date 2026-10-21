import { test } from "@clice/tools/testing";

// A thousand nested instantiations take clang several megabytes of stack,
// more than the 2 MB main thread a Windows linker gives by default: clang's
// own guard moves to a fresh stack only as its usage nears 8 MB.
test(async (t) => {
    await t.cli(["inspect", "folding-range", "main.cpp"]);
});
