import { answers } from "@clice/tools/testing";

// main.cpp reuses core.dir's precompiled header, built without
// -DBUILD_STATIC, as CMake's REUSE_FROM does across targets (LLVM's tools
// and unittests over LLVMCore's): ABI is what that PCH defines, as the
// compiler sees it, not what main.cpp's own flags would make of the header.
answers(async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.hover(m));
    }
});
