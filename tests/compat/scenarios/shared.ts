/// What the scenario files share: the compilers several of them name and
/// the CMake builds most of them run.

import * as path from "node:path";
import type { FileExpectation, Scenario } from "@clice/tools/compat/scenario";
import { REPO_ROOT } from "@clice/tools/repo";

export const GCC = "/usr/bin/gcc";
export const GXX = "/usr/bin/g++";
export const CLANG = "/usr/bin/clang";
export const CLANGXX = "/usr/bin/clang++";

export const BOTH: Record<string, FileExpectation> = { "src/main.cpp": {}, "src/util.c": {} };

export function cmake(
    generator: string,
    cc: string,
    cxx: string,
    ...extra: string[]
): Scenario["build"] {
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

export function cmakeNinja(
    name: string,
    platforms: NodeJS.Platform[],
    cc: string,
    cxx: string,
    options: { msvc?: boolean; toolset?: string; extra?: string[] } = {},
): Scenario {
    return {
        name,
        platforms,
        msvc: options.msvc === true,
        ...(options.toolset === undefined ? {} : { toolset: options.toolset }),
        requires: ["cmake", "ninja", cc, cxx],
        build: cmake("Ninja", cc, cxx, ...(options.extra ?? [])),
        files: BOTH,
    };
}

/// Conda's GCC and clang and Intel's oneAPI compilers of the pixi
/// `toolchains` environment, run without its activation as a build outside
/// it runs them.
export const CONDA = path.join(REPO_ROOT, ".pixi", "envs", "toolchains", "bin");

export const LLVM_WINDOWS = "C:/Program Files/LLVM/bin";
export const MINGW = "C:/mingw64/bin";

/// MSYS2's UCRT GCC and its clang environment, whose clang targets
/// windows-gnu with its own libc++ and resource tree.
export const MSYS2_UCRT = "C:/msys64/ucrt64/bin";
export const MSYS2_CLANG = "C:/msys64/clang64/bin";

/// Cygwin and MSYS2's own tools, whose builds write POSIX paths: Cygwin's
/// CMake records `/usr/bin/cc`, a Cygwin symlink, for C.
export const CYGWIN = "C:/cygwin64/bin";
export const MSYS2 = "C:/msys64/usr/bin";
