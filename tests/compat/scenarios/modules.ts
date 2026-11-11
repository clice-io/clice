/// C++20 modules: GCC's and MSVC's, and `import std`.

import * as os from "node:os";
import * as path from "node:path";
import type { Scenario } from "@clice/tools/compat/scenario";
import { BOTH, cmake, cmakeNinja, CONDA, MSYS2_CLANG } from "./shared.ts";

/// CMake 4.5, the first whose `import std` needs no experimental gate,
/// installed by the CI in the home directory.
const CMAKE_45 = path.join(
    os.homedir(),
    "cmake-4.5",
    "bin",
    process.platform === "win32" ? "cmake.exe" : "cmake",
);

/// A CMake 4.5 build of the project with its `import std` unit: clice
/// builds the standard library's module from its database entry.
/// use_std.cpp is left to the lint: it includes no library header, so it
/// has no version macros to compare.
function importStd(name: string, platforms: NodeJS.Platform[], cc: string, cxx: string): Scenario {
    return {
        name,
        platforms,
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
            ],
            [CMAKE_45, "--build", "build"],
        ],
        files: BOTH,
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
    // `import std` from the libc++ of a clang targeting windows-gnu, as
    // LLVM-MinGW's builds write it.
    importStd(
        "cmake ninja msys2 clang import std",
        ["win32"],
        `${MSYS2_CLANG}/clang.exe`,
        `${MSYS2_CLANG}/clang++.exe`,
    ),
];
