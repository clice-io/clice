import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNQUERIED = "The compiler could not be queried";

// What the build writes leaves the command: the object file and the
// compile-only switch. The compilers do not exist, so each command prints
// as clice reads it.
test("objects", { errors: [MISSING, UNQUERIED] }, async (t) => {
    await t.cli("command convert -- missing-g++ main.cpp");
    await t.cli("command convert -- missing-clang++ -c main.cpp");
    await t.cli("command convert -- missing-clang++ -o main.o main.cpp");
    await t.cli("command convert -- missing-clang++ -c -o main.o main.cpp");
    await t.cli("command convert -- cl-missing.exe /c /Fomain.cpp.o main.cpp");
    await t.cli("command convert -- CL-missing.exe /FIfoo.h /c main.cpp");
});

// The precompiled header a CMake target builds or uses leaves; the
// header it forces in stays.
test("precompiled headers", { errors: [MISSING, UNQUERIED] }, async (t) => {
    await t.cli(
        "command convert -- missing-g++ -std=gnu++20 -Winvalid-pch -include cmake_pch.hxx -o main.cpp.o -c main.cpp",
    );
    await t.cli(
        "command convert -- missing-clang++ -Winvalid-pch -Xclang -include-pch -Xclang cmake_pch.hxx.pch -Xclang -include -Xclang cmake_pch.hxx -o main.cpp.o -c main.cpp",
    );
    await t.cli(
        "command convert -- cl-missing.exe /Yufoo.h /FIfoo.h /Fpfoo.h_v143.pch /c /Fomain.cpp.o main.cpp",
    );
    await t.cli(
        "command convert -- cl-missing.exe /Ycfoo.h /FIfoo.h /Fpfoo.h_v143.pch /c /Fomain.cpp.o main.cpp",
    );
});

// The dependency file a build asks for leaves, also when Kbuild hands it
// to the preprocessor through -Wp; what else a -Wp hands over stays.
test("dependency files", { errors: [MISSING, UNQUERIED] }, async (t) => {
    await t.cli("command convert -- missing-clang++ -std=c++20 -MD -MF main.d -MT main.o main.cpp");
    await t.cli("command convert -- missing-gcc -Wp,-MMD,arch/x86/.main.o.d -std=gnu11 main.cpp");
    await t.cli(
        "command convert -- missing-clang++ -Wp,-MD,main.d -Wp,-MT,main.o,-MP -Wp,-MQmain.o main.cpp",
    );
    await t.cli("command convert -- missing-g++ -Wp,-D_FORTIFY_SOURCE=2 main.cpp");
    await t.cli("command convert -- missing-g++ -Wp,-MMD,main.d,-DNDEBUG main.cpp");
    await t.cli("command convert -- missing-g++ -Wp,-fclice-unknown main.cpp");
    await t.cli("command convert -- missing-g++ -Wp,-include -Wp,config.h main.cpp");
});
