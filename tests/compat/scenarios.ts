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

function cmakeNinja(
    name: string,
    platforms: NodeJS.Platform[],
    cc: string,
    cxx: string,
    options: { msvc?: boolean; extra?: string[]; unsupported?: string } = {},
): Scenario {
    return {
        name,
        platforms,
        ...(options.unsupported === undefined ? {} : { unsupported: options.unsupported }),
        msvc: options.msvc === true,
        requires: ["cmake", "ninja", cc, cxx],
        build: cmake("Ninja", cc, cxx, ...(options.extra ?? [])),
        files: BOTH,
    };
}

const LLVM_WINDOWS = "C:/Program Files/LLVM/bin";
const MINGW = "C:/mingw64/bin";

export const SCENARIOS: Scenario[] = [
    cmakeNinja("cmake ninja gcc", ["linux"], GCC, GXX),
    // Versioned driver names, side by side as distributions install them.
    cmakeNinja("cmake ninja gcc-12", ["linux"], "/usr/bin/gcc-12", "/usr/bin/g++-12"),
    cmakeNinja("cmake ninja gcc-14", ["linux"], "/usr/bin/gcc-14", "/usr/bin/g++-14"),
    cmakeNinja("cmake ninja clang-16", ["linux"], "/usr/bin/clang-16", "/usr/bin/clang++-16"),
    cmakeNinja("cmake ninja clang-17", ["linux"], "/usr/bin/clang-17", "/usr/bin/clang++-17"),
    cmakeNinja(
        "cmake ninja riscv64 cross",
        ["linux"],
        "/usr/bin/riscv64-linux-gnu-gcc",
        "/usr/bin/riscv64-linux-gnu-g++",
        { extra: ["-DCMAKE_SYSTEM_NAME=Linux", "-DCMAKE_SYSTEM_PROCESSOR=riscv64"] },
    ),
    cmakeNinja("cmake ninja apple clang", ["darwin"], "/usr/bin/clang", "/usr/bin/clang++", {
        unsupported: "Apple clang defaults C++ to C++14; clice fills in its own C++17 (#354)",
    }),
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
        { unsupported: "GCC 15 defaults C to gnu23; clice keeps gnu17" },
    ),
    cmakeNinja(
        "cmake ninja homebrew llvm",
        ["darwin"],
        "/opt/homebrew/opt/llvm@18/bin/clang",
        "/opt/homebrew/opt/llvm@18/bin/clang++",
    ),
    cmakeNinja("cmake ninja msvc", ["win32"], "cl", "cl", { msvc: true }),
    cmakeNinja(
        "cmake ninja clang-cl",
        ["win32"],
        `${LLVM_WINDOWS}/clang-cl.exe`,
        `${LLVM_WINDOWS}/clang-cl.exe`,
        { msvc: true },
    ),
    cmakeNinja(
        "cmake ninja llvm clang",
        ["win32"],
        `${LLVM_WINDOWS}/clang.exe`,
        `${LLVM_WINDOWS}/clang++.exe`,
    ),
    cmakeNinja("cmake ninja mingw gcc", ["win32"], `${MINGW}/gcc.exe`, `${MINGW}/g++.exe`, {
        unsupported:
            "MinGW's GCC 15 defines _REENTRANT and defaults C to gnu23; clice does neither",
    }),
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
    {
        name: "cmake ninja zig",
        platforms: ["linux"],
        unsupported:
            "zig targets the host CPU by default, clice the generic one; " +
            "asking zig for its command fails (#98)",
        requires: ["cmake", "ninja", "zig"],
        build: cmake("Ninja", "zig;cc", "zig;c++"),
        files: BOTH,
    },
    {
        name: "emcmake emscripten",
        platforms: ["linux"],
        unsupported:
            "clice takes em++ for a host compiler: wrong target, data model and " +
            "default standard, no sysroot headers (#569)",
        requires: ["emcmake", "emcc", "em++", "cmake", "ninja"],
        build: [
            ["emcmake", "cmake", "-S", ".", "-B", "build", "-G", "Ninja"],
            ["cmake", "--build", "build"],
        ],
        files: BOTH,
    },
    {
        name: "make bear arm-none-eabi",
        platforms: ["linux"],
        unsupported: "clice finds no header search paths for the bare-metal GCC",
        requires: ["bear", "make", "/usr/bin/arm-none-eabi-gcc", "/usr/bin/arm-none-eabi-g++"],
        build: [
            [
                "bear",
                "--",
                "make",
                "CC=/usr/bin/arm-none-eabi-gcc",
                "CXX=/usr/bin/arm-none-eabi-g++",
                "CFLAGS=-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 --specs=nano.specs",
                "CXXFLAGS=-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 --specs=nano.specs -fno-exceptions",
            ],
        ],
        files: BOTH,
    },
];
