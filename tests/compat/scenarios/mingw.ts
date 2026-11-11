/// The windows-gnu family: MinGW's GCC and clangs targeting it, MSYS2's and
/// Cygwin's compilers.

import type { Scenario } from "@clice/tools/compat/scenario";
import {
    BOTH,
    cmakeNinja,
    LLVM_WINDOWS,
    MINGW,
    MSYS2_UCRT,
    MSYS2_CLANG,
    CYGWIN,
    MSYS2,
} from "./shared.ts";

/// CMake run inside Cygwin or MSYS2 (`bin`), in a project directory whose
/// name has spaces and letters past ASCII.
function cmakePosix(name: string, bin: string, ...extra: string[]): Scenario {
    return {
        name,
        platforms: ["win32"],
        requires: [`${bin}/cmake.exe`, `${bin}/ninja.exe`, `${bin}/gcc.exe`, `${bin}/g++.exe`],
        path: [bin],
        directory: "compat 项目 ü",
        posix: bin,
        build: [
            [`${bin}/cmake.exe`, "-S", ".", "-B", "build", "-G", "Ninja", ...extra],
            [`${bin}/cmake.exe`, "--build", "build"],
        ],
        files: BOTH,
    };
}

export const SCENARIOS: Scenario[] = [
    // The runner's LLVM, of another version than clice's, for MinGW, whose
    // headers and libraries it finds beside MinGW's gcc.
    {
        ...cmakeNinja(
            "cmake ninja llvm clang mingw",
            ["win32"],
            `${LLVM_WINDOWS}/clang.exe`,
            `${LLVM_WINDOWS}/clang++.exe`,
            {
                extra: [
                    "-DCMAKE_C_COMPILER_TARGET=x86_64-w64-mingw32",
                    "-DCMAKE_CXX_COMPILER_TARGET=x86_64-w64-mingw32",
                ],
            },
        ),
        requires: [
            "cmake",
            "ninja",
            `${LLVM_WINDOWS}/clang.exe`,
            `${LLVM_WINDOWS}/clang++.exe`,
            `${MINGW}/gcc.exe`,
        ],
    },
    cmakeNinja("cmake ninja mingw gcc", ["win32"], `${MINGW}/gcc.exe`, `${MINGW}/g++.exe`),
    {
        ...cmakeNinja(
            "cmake ninja msys2 ucrt gcc",
            ["win32"],
            `${MSYS2_UCRT}/gcc.exe`,
            `${MSYS2_UCRT}/g++.exe`,
        ),
        path: [MSYS2_UCRT],
    },
    cmakeNinja(
        "cmake ninja msys2 clang",
        ["win32"],
        `${MSYS2_CLANG}/clang.exe`,
        `${MSYS2_CLANG}/clang++.exe`,
    ),
    {
        ...cmakePosix("cygwin cmake ninja gcc", CYGWIN),
        files: { "src/main.cpp": {}, "src/util.c": { recorded: [["/usr/bin/cc"]] } },
    },
    // CMake forces the PCH's header in by its POSIX path.
    {
        ...cmakePosix("cygwin cmake ninja gcc precompiled headers", CYGWIN, "-DCOMPAT_PCH=ON"),
        files: {
            "src/main.cpp": { recorded: [["-Winvalid-pch"]] },
            "src/util.c": { recorded: [["/usr/bin/cc"]] },
        },
    },
    cmakePosix("msys2 cmake ninja gcc", MSYS2),
];
