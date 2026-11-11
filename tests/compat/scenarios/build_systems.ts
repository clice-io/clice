/// The build systems and what they write: make captured with bear, meson,
/// xmake, bazel, response files, launchers, precompiled headers.

import type { Scenario } from "@clice/tools/compat/scenario";
import { GCC, GXX, CLANG, CLANGXX, BOTH, cmake } from "./shared.ts";

export const SCENARIOS: Scenario[] = [
    {
        // Every C++ unit loads the target's PCH, which clice builds itself
        // from cmake_pch.hxx.cxx: CMake's own is clang 17's.
        name: "cmake ninja clang-17 precompiled headers",
        platforms: ["linux"],
        requires: ["cmake", "ninja", "/usr/bin/clang-17", "/usr/bin/clang++-17"],
        build: cmake("Ninja", "/usr/bin/clang-17", "/usr/bin/clang++-17", "-DCOMPAT_PCH=ON"),
        files: { "src/main.cpp": { recorded: [["-Xclang", "-include-pch"]] }, "src/util.c": {} },
    },
    {
        // GCC finds its .gch beside the header the command forces in.
        name: "cmake ninja gcc precompiled headers",
        platforms: ["linux"],
        requires: ["cmake", "ninja", GCC, GXX],
        build: cmake("Ninja", GCC, GXX, "-DCOMPAT_PCH=ON"),
        files: { "src/main.cpp": { recorded: [["-Winvalid-pch"]] }, "src/util.c": {} },
    },
    {
        name: "cmake make clang",
        platforms: ["linux"],
        requires: ["cmake", "make", CLANG, CLANGXX],
        build: cmake("Unix Makefiles", CLANG, CLANGXX),
        files: BOTH,
    },
    {
        // CMake keeps its compiler launcher out of the database; meson
        // writes the ccache its native file names in front of the compiler.
        name: "meson ccache launcher",
        platforms: ["linux"],
        requires: ["meson", "ninja", "ccache", GCC, GXX],
        build: [
            ["meson", "setup", "build", "--native-file", "ccache.ini"],
            ["ninja", "-C", "build"],
        ],
        files: {
            "src/main.cpp": { recorded: [["ccache", GXX]], contains: [[GXX, "-cc1"]] },
            "src/util.c": { recorded: [["ccache", GCC]], contains: [[GCC, "-cc1"]] },
        },
    },
    {
        name: "cmake response files",
        platforms: ["linux"],
        requires: ["cmake", "make", GCC, GXX],
        build: cmake(
            "Unix Makefiles",
            GCC,
            GXX,
            "-DCMAKE_C_USE_RESPONSE_FILE_FOR_INCLUDES=ON",
            "-DCMAKE_CXX_USE_RESPONSE_FILE_FOR_INCLUDES=ON",
        ),
        files: {
            "src/main.cpp": {
                recorded: [["@CMakeFiles/compat.dir/includes_CXX.rsp"]],
                contains: [["-I", "${root}/include"]],
            },
            "src/util.c": {
                recorded: [["@CMakeFiles/compat.dir/includes_C.rsp"]],
                contains: [["-I", "${root}/include"]],
            },
        },
    },
    {
        name: "xmake gcc",
        platforms: ["linux"],
        requires: ["xmake", GCC, GXX],
        build: [
            ["xmake", "config", "--yes", `--cc=${GCC}`, `--cxx=${GXX}`],
            ["xmake", "build", "--yes"],
            ["xmake", "project", "--kind=compile_commands"],
        ],
        files: BOTH,
    },
    {
        name: "make bear gcc flags",
        platforms: ["linux"],
        requires: ["bear", "make", GCC, GXX],
        build: [
            [
                "bear",
                "--",
                "make",
                `CC=${GCC}`,
                `CXX=${GXX}`,
                "CXXFLAGS=-std=gnu++20 -O2 -g -mtune=intel -fno-exceptions -fno-rtti -pthread -Werror -Werror=shadow -MD -MF build/main.d",
                "CFLAGS=-std=c11 -Os -ffast-math -funsigned-char -march=x86-64-v3 -ffunction-sections -flto -fsanitize=address",
            ],
        ],
        // Outputs, dependency files, debug info and pure codegen switches
        // are dropped, and so is a CPU name only GCC knows; what changes the
        // parse stays (the macro check sees
        // -ffast-math, -funsigned-char, -march and -fno-exceptions), a
        // relative include directory anchored at the entry's directory, and
        // -flto, which the driver requires for flags such as -fsanitize=cfi.
        // The warnings GCC makes errors stay warnings: clang warns of more.
        files: {
            "src/main.cpp": {
                contains: [
                    ["-std=gnu++20"],
                    ["-fno-rtti"],
                    ["-pthread"],
                    ["-I", "${root}/include"],
                    ["-Wshadow"],
                ],
                excludes: [
                    "-dependency-file",
                    "-debug-info-kind=constructor",
                    "intel",
                    "-Werror",
                    "-Werror=shadow",
                ],
            },
            "src/util.c": {
                contains: [["-std=c11"], ["-fsanitize=address"], ["-flto=full"]],
                excludes: ["-ffunction-sections"],
            },
        },
    },
    {
        // GCC knows these by names clang's own differ from
        // (-faligned-allocation, -ftrigraphs, -flto=full), which GCC
        // rejects: the query gives them as the build writes them.
        name: "make bear gcc aliases",
        platforms: ["linux"],
        requires: ["bear", "make", GCC, GXX],
        build: [
            [
                "bear",
                "--",
                "make",
                `CC=${GCC}`,
                `CXX=${GXX}`,
                "CXXFLAGS=-std=gnu++14 -faligned-new -flto=auto",
                "CFLAGS=-std=gnu11 -trigraphs -flto",
            ],
        ],
        files: {
            "src/main.cpp": { recorded: [["-faligned-new"], ["-flto=auto"]] },
            "src/util.c": { recorded: [["-trigraphs"], ["-flto"]] },
        },
    },
    {
        // Semantic flags an external clang turns into frontend options come
        // back from the query instead of being dropped with the toolchain's;
        // a clang's warnings as errors stay errors.
        name: "make bear clang flags",
        platforms: ["linux"],
        requires: ["bear", "make", CLANG, CLANGXX],
        build: [
            [
                "bear",
                "--",
                "make",
                `CC=${CLANG}`,
                `CXX=${CLANGXX}`,
                "CXXFLAGS=-std=c++23 -fms-extensions -Wno-everything -Werror",
            ],
        ],
        files: {
            "src/main.cpp": {
                contains: [["-std=c++23"], ["-fms-extensions"], ["-Wno-everything"], ["-Werror"]],
            },
            "src/util.c": {},
        },
    },
    {
        name: "bazel hedron gcc",
        platforms: ["linux"],
        requires: ["bazel", GCC],
        build: [
            ["bazel", "run", "@hedron_compile_commands//:refresh_all"],
            ["bazel", "shutdown"],
        ],
        files: BOTH,
    },
];
