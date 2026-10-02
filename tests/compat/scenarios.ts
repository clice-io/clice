/// The compatibility matrix. Each scenario names its compiler by absolute
/// path where the system has several, so the database records the
/// toolchain under test rather than whatever PATH finds first.

import type { FileExpectation, Scenario } from "@clice/tools/compat/scenario";

const GCC = "/usr/bin/gcc";
const GXX = "/usr/bin/g++";
const CLANG = "/usr/bin/clang";
const CLANGXX = "/usr/bin/clang++";

const BOTH: Record<string, FileExpectation> = { "src/main.cpp": {}, "src/util.c": {} };

function cmake(generator: string, cc: string, cxx: string, ...extra: string[]): Scenario["build"] {
    return [
        [
            "cmake",
            "-S",
            ".",
            "-B",
            "build",
            "-G",
            generator,
            `-DCMAKE_C_COMPILER=${cc}`,
            `-DCMAKE_CXX_COMPILER=${cxx}`,
            ...extra,
        ],
        ["cmake", "--build", "build"],
    ];
}

export const SCENARIOS: Scenario[] = [
    {
        name: "cmake ninja gcc",
        platforms: ["linux"],
        requires: ["cmake", "ninja", GCC, GXX],
        build: cmake("Ninja", GCC, GXX),
        files: BOTH,
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
        name: "meson mingw cross",
        platforms: ["linux"],
        requires: ["meson", "ninja", "x86_64-w64-mingw32-gcc", "x86_64-w64-mingw32-g++"],
        build: [
            ["meson", "setup", "build", "--cross-file", "mingw.ini"],
            ["ninja", "-C", "build"],
        ],
        files: BOTH,
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
                "CXXFLAGS=-std=gnu++20 -O2 -g -fno-exceptions -fno-rtti -pthread -MD -MF build/main.d",
                "CFLAGS=-std=c11 -Os -ffast-math -funsigned-char -march=x86-64-v3 -ffunction-sections -flto -fsanitize=address",
            ],
        ],
        // Outputs, dependency files, debug info and pure codegen switches
        // are dropped; what changes the parse stays (the macro check sees
        // -ffast-math, -funsigned-char, -march and -fno-exceptions), a
        // relative include directory anchored at the entry's directory.
        files: {
            "src/main.cpp": {
                contains: [
                    ["-std=gnu++20"],
                    ["-fno-rtti"],
                    ["-pthread"],
                    ["-I", "${root}/include"],
                ],
                excludes: ["-dependency-file", "-debug-info-kind=constructor"],
            },
            "src/util.c": {
                contains: [["-std=c11"], ["-fsanitize=address"]],
                excludes: ["-ffunction-sections", "-flto=full"],
            },
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
