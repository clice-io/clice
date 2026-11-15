/// C++20 modules: GCC's and MSVC's, and `import std` from each standard
/// library.

import * as os from "node:os";
import * as path from "node:path";
import type { Answer, Scenario } from "@clice/tools/compat/scenario";
import { BOTH, cmake, cmakeNinja, CONDA, MSYS2_CLANG } from "./shared.ts";

/// CMake 4.5, the first whose `import std` needs no experimental gate,
/// installed by the CI in the home directory.
const CMAKE_45 = path.join(
    os.homedir(),
    "cmake-4.5",
    "bin",
    process.platform === "win32" ? "cmake.exe" : "cmake",
);

/// What the editor answers in the units importing `std`, under every
/// standard library: a name of the library leads into its headers, the
/// project's own into its module's interface and partition, from another
/// target.
const STD_ANSWERS: Record<string, Answer[]> = {
    "std/use_shapes.cpp": [
        { at: "vector<compat", definition: "vector" },
        { at: "Circle>", definition: "std/geometry.cppm" },
        { at: "grow(", definition: "std/shapes.cppm" },
        { at: "describe(", hover: "compat" },
    ],
    "std/shapes.cpp": [
        { at: "area()", hover: "compat::Circle" },
        { at: "format(", definition: "format" },
    ],
    "std/geometry.cppm": [{ at: "pi *", hover: "std::numbers" }],
    "std/mixed.cpp": [
        { at: "sort(", hover: "sort" },
        { at: "values.front", offset: 7, completion: "front" },
    ],
    "std/use_compat.cpp": [{ at: "strlen", definition: "string.h" }],
};

/// A CMake 4.5 build of the project with its units importing `std`, and
/// the units `options` add: clice builds the standard library's module
/// from its database entries. The units are left to the lint and the
/// editor's answers: they include no library header, so they have no
/// version macros to compare.
function importStd(
    name: string,
    platforms: NodeJS.Platform[],
    cc: string,
    cxx: string,
    options: { msvc?: boolean; extra?: string[]; answers?: Record<string, Answer[]> } = {},
): Scenario {
    return {
        name,
        platforms,
        msvc: options.msvc === true,
        requires: [CMAKE_45, "ninja", cc, cxx],
        build: [
            [
                CMAKE_45,
                "-S",
                ".",
                "-B",
                "build",
                "-G",
                "Ninja",
                `-DCMAKE_C_COMPILER=${cc}`,
                `-DCMAKE_CXX_COMPILER=${cxx}`,
                "-DCOMPAT_IMPORT_STD=ON",
                ...(options.extra ?? []),
            ],
            [CMAKE_45, "--build", "build"],
        ],
        files: BOTH,
        answers: { ...STD_ANSWERS, ...options.answers },
    };
}

export const SCENARIOS: Scenario[] = [
    {
        // GCC 15 spells C++20 modules -fmodules, which to clang would mean
        // its header modules.
        name: "make bear conda gcc modules",
        platforms: ["linux"],
        requires: ["bear", "make", `${CONDA}/x86_64-conda-linux-gnu-g++`],
        build: [
            [
                "bear",
                "--",
                "make",
                "build/main.o",
                `CXX=${CONDA}/x86_64-conda-linux-gnu-g++`,
                "CXXFLAGS=-std=c++20 -fmodules",
            ],
        ],
        files: { "src/main.cpp": { recorded: [["-fmodules"]], excludes: ["-fmodules"] } },
    },
    {
        // CMake scans GCC's module units and hands each its own module
        // mapper, a file of the unit that every command must leave out,
        // with GCC's dependency format; clice builds the module itself.
        name: "cmake ninja conda gcc modules",
        platforms: ["linux"],
        requires: [
            "cmake",
            "ninja",
            `${CONDA}/x86_64-conda-linux-gnu-gcc`,
            `${CONDA}/x86_64-conda-linux-gnu-g++`,
        ],
        build: cmake(
            "Ninja",
            `${CONDA}/x86_64-conda-linux-gnu-gcc`,
            `${CONDA}/x86_64-conda-linux-gnu-g++`,
            "-DCOMPAT_MODULES=ON",
        ),
        files: {
            "src/greeting.cppm": {
                recorded: [
                    ["-fmodules-ts"],
                    ["-fmodule-mapper=CMakeFiles/compat_modules.dir/src/greeting.cppm.o.modmap"],
                    ["-fdeps-format=p1689r5"],
                ],
                excludes: ["-fmodules-ts", "-fdeps-format=p1689r5"],
            },
            "src/use_greeting.cpp": {
                recorded: [
                    ["-fmodule-mapper=CMakeFiles/compat_modules.dir/src/use_greeting.cpp.o.modmap"],
                ],
                excludes: ["-fmodules-ts", "-fdeps-format=p1689r5"],
            },
        },
    },
    {
        // CMake hands each of cl's module units a response file of its own
        // (its modmap): what the unit is, the interface it writes and those
        // it reads, none of which the command clice compiles keeps; clice
        // builds the module itself.
        ...cmakeNinja("cmake ninja msvc modules", ["win32"], "cl", "cl", {
            msvc: true,
            extra: ["-DCOMPAT_MODULES=ON"],
        }),
        files: {
            "src/greeting.cppm": {
                excludes: ["-interface", "-ifcOutput", "-reference", "/reference"],
            },
            "src/use_greeting.cpp": { excludes: ["-reference", "/reference"] },
        },
    },
    // `import std` from GCC 15's libstdc++.
    importStd(
        "cmake ninja conda gcc import std",
        ["linux"],
        `${CONDA}/x86_64-conda-linux-gnu-gcc`,
        `${CONDA}/x86_64-conda-linux-gnu-g++`,
    ),
    // A second importer built with -pthread, for which CMake builds `std`
    // again: an importer reads the build of its own language options
    // (#588).
    importStd(
        "cmake ninja conda gcc import std, two option sets",
        ["linux"],
        `${CONDA}/x86_64-conda-linux-gnu-gcc`,
        `${CONDA}/x86_64-conda-linux-gnu-g++`,
        { extra: ["-DCOMPAT_STD_PTHREAD=ON"] },
    ),
    // `import std` from the distribution's libc++, whose module sources
    // its libc++-dev package installs.
    importStd(
        "cmake ninja clang libc++ import std",
        ["linux"],
        "/usr/bin/clang-18",
        "/usr/bin/clang++-18",
        {
            extra: ["-DCMAKE_CXX_FLAGS=-stdlib=libc++"],
        },
    ),
    // `import std` from MSVC's STL, and a target precompiling a header for
    // its module unit and its importer alike (#1078).
    importStd("cmake ninja msvc import std", ["win32"], "cl", "cl", {
        msvc: true,
        extra: ["-DCOMPAT_STD_PCH=ON"],
        answers: {
            "std/pch_unit.cppm": [{ at: "vector<int>", definition: "vector" }],
            "std/pch_user.cpp": [{ at: "pch_unit()", definition: "std/pch_unit.cppm" }],
        },
    }),
    // `import std` from the libc++ of a clang targeting windows-gnu, as
    // LLVM-MinGW's builds write it.
    importStd(
        "cmake ninja msys2 clang import std",
        ["win32"],
        `${MSYS2_CLANG}/clang.exe`,
        `${MSYS2_CLANG}/clang++.exe`,
    ),
];
