/// Each platform's own compilers, as their installations ship them.

import type { Scenario } from "@clice/tools/compat/scenario";
import { GCC, GXX, CLANG, CLANGXX, cmakeNinja, CONDA, LLVM_WINDOWS } from "./shared.ts";

export const SCENARIOS: Scenario[] = [
    cmakeNinja("cmake ninja gcc", ["linux"], GCC, GXX),
    // Versioned driver names, side by side as distributions install them.
    cmakeNinja("cmake ninja gcc-12", ["linux"], "/usr/bin/gcc-12", "/usr/bin/g++-12"),
    cmakeNinja("cmake ninja gcc-14", ["linux"], "/usr/bin/gcc-14", "/usr/bin/g++-14"),
    // The distribution's `cc` and `c++`, which name GCC by alternatives.
    cmakeNinja("cmake ninja cc alias", ["linux"], "/usr/bin/cc", "/usr/bin/c++"),
    cmakeNinja("cmake ninja clang-16", ["linux"], "/usr/bin/clang-16", "/usr/bin/clang++-16"),
    cmakeNinja("cmake ninja clang libc++", ["linux"], CLANG, CLANGXX, {
        extra: ["-DCMAKE_CXX_FLAGS=-stdlib=libc++"],
    }),
    cmakeNinja("cmake ninja clang-17", ["linux"], "/usr/bin/clang-17", "/usr/bin/clang++-17"),
    // Conda's GCC compiles against its own sysroot's glibc.
    cmakeNinja(
        "cmake ninja conda gcc",
        ["linux"],
        `${CONDA}/x86_64-conda-linux-gnu-gcc`,
        `${CONDA}/x86_64-conda-linux-gnu-g++`,
    ),
    cmakeNinja("cmake ninja conda clang", ["linux"], `${CONDA}/clang`, `${CONDA}/clang++`),
    cmakeNinja("cmake ninja apple clang", ["darwin"], "/usr/bin/clang", "/usr/bin/clang++"),
    // macOS's `cc` and `c++` are clang.
    cmakeNinja("cmake ninja apple cc alias", ["darwin"], "/usr/bin/cc", "/usr/bin/c++"),
    cmakeNinja(
        "cmake ninja homebrew gcc-13",
        ["darwin"],
        "/opt/homebrew/bin/gcc-13",
        "/opt/homebrew/bin/g++-13",
    ),
    cmakeNinja(
        "cmake ninja homebrew gcc",
        ["darwin"],
        "/opt/homebrew/bin/gcc-14",
        "/opt/homebrew/bin/g++-14",
    ),
    cmakeNinja(
        "cmake ninja homebrew gcc-15",
        ["darwin"],
        "/opt/homebrew/bin/gcc-15",
        "/opt/homebrew/bin/g++-15",
    ),
    cmakeNinja(
        "cmake ninja homebrew llvm",
        ["darwin"],
        "/opt/homebrew/opt/llvm@18/bin/clang",
        "/opt/homebrew/opt/llvm@18/bin/clang++",
    ),
    cmakeNinja("cmake ninja msvc", ["win32"], "cl", "cl", { msvc: true }),
    // An older toolset beside the newest: its cl.exe compiles with its own
    // headers (`_MSVC_STL_VERSION`), not the newest toolset's.
    cmakeNinja("cmake ninja msvc 14.29 toolset", ["win32"], "cl", "cl", {
        msvc: true,
        toolset: "14.29",
    }),
    // The newest standard, cl's conformance switches, and a Debug build's
    // runtime library. cl's warnings as errors stay warnings, clang-cl's
    // errors.
    {
        ...cmakeNinja("cmake ninja msvc conformance", ["win32"], "cl", "cl", {
            msvc: true,
            extra: [
                "-DCMAKE_BUILD_TYPE=Debug",
                "-DCMAKE_CXX_FLAGS=/EHsc /std:c++latest /Zc:__cplusplus /permissive- /WX",
            ],
        }),
        files: { "src/main.cpp": { excludes: ["-Werror"] }, "src/util.c": {} },
    },
    cmakeNinja(
        "cmake ninja clang-cl",
        ["win32"],
        `${LLVM_WINDOWS}/clang-cl.exe`,
        `${LLVM_WINDOWS}/clang-cl.exe`,
        { msvc: true },
    ),
    {
        ...cmakeNinja(
            "cmake ninja clang-cl conformance",
            ["win32"],
            `${LLVM_WINDOWS}/clang-cl.exe`,
            `${LLVM_WINDOWS}/clang-cl.exe`,
            { msvc: true, extra: ["-DCMAKE_CXX_FLAGS=/EHsc /std:c++latest /permissive- /WX"] },
        ),
        files: { "src/main.cpp": { contains: [["-Werror"]] }, "src/util.c": {} },
    },
    cmakeNinja(
        "cmake ninja llvm clang",
        ["win32"],
        `${LLVM_WINDOWS}/clang.exe`,
        `${LLVM_WINDOWS}/clang++.exe`,
    ),
];
