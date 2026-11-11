/// Offload compilers: nvcc and SYCL.

import * as path from "node:path";
import type { FileExpectation, Scenario } from "@clice/tools/compat/scenario";
import { REPO_ROOT } from "@clice/tools/repo";
import { BOTH, cmake, cmakeNinja, CONDA } from "./shared.ts";

/// The CUDA toolkit of the pixi `cuda` environment, run without its
/// activation, so nvcc picks the system GCC as its host compiler.
const NVCC = path.join(REPO_ROOT, ".pixi", "envs", "cuda", "bin", "nvcc");

/// nvcc builds of the CUDA part, captured with bear.
function nvcc(
    name: string,
    config: string | undefined,
    files: Record<string, FileExpectation>,
): Scenario {
    return {
        name,
        platforms: ["linux"],
        requires: ["bear", "make", NVCC],
        build: [["bear", "--", "make", "cuda", `NVCC=${NVCC}`]],
        ...(config === undefined ? {} : { config }),
        files,
    };
}

/// Where the pixi `toolchains` environment has its compilers on Windows:
/// Intel's oneAPI ones and nvcc.
const CONDA_WINDOWS = path.join(REPO_ROOT, ".pixi", "envs", "toolchains", "Library", "bin");

/// A SYCL source compiles in a device job, then the host's: clice reads it
/// as the host job.
const SYCL_FILES: Record<string, FileExpectation> = {
    ...BOTH,
    "sycl/kernel.cpp": {
        recorded: [["-fsycl"]],
        contains: [["-fsycl-is-host"]],
        excludes: ["-fsycl-is-device"],
    },
};

export const SCENARIOS: Scenario[] = [
    // A CUDA source parses in the device view, a host-language source nvcc
    // compiles stays a plain host compile under nvcc's macros. Both include
    // libcu++, in CUDA 13's include/cccl, which only nvcc's host line names.
    nvcc("make bear nvcc", undefined, {
        "cuda/kernel.cu": { contains: [["-fcuda-is-device"], ["CUDA_DOUBLE_MATH_FUNCTIONS"]] },
        "cuda/host.cpp": { excludes: ["-fcuda-is-device"] },
    }),
    // A rule appending a view selector picks the host pass, with the
    // defines that only nvcc's device line carries gone too.
    nvcc(
        "make bear nvcc host view",
        '[[rules]]\npatterns = ["cuda/kernel.cu"]\nappend = ["--cuda-host-only"]\n',
        {
            "cuda/kernel.cu": { excludes: ["-fcuda-is-device", "CUDA_DOUBLE_MATH_FUNCTIONS"] },
            "cuda/host.cpp": {},
        },
    ),
    // A rule appending a special architecture resolves through nvcc
    // instead of falling back to clang's default sm_52.
    nvcc(
        "make bear nvcc arch rule",
        '[[rules]]\npatterns = ["cuda/kernel.cu"]\nappend = ["-arch=all"]\n',
        { "cuda/kernel.cu": { excludes: ["sm_52"] }, "cuda/host.cpp": {} },
    ),
    {
        ...cmakeNinja("cmake ninja icpx sycl", ["linux"], `${CONDA}/icx`, `${CONDA}/icpx`, {
            extra: ["-DCOMPAT_SYCL=ON"],
        }),
        files: SYCL_FILES,
    },
    {
        // icx takes cl's options on Windows. It links only with the
        // libraries its activation adds to LIB: the build compiles the
        // SYCL library alone, and CMake checks the compilers without
        // linking.
        name: "cmake ninja icx sycl",
        platforms: ["win32"],
        msvc: true,
        requires: ["cmake", "ninja", `${CONDA_WINDOWS}/icx.exe`],
        build: [
            ...cmake(
                "Ninja",
                `${CONDA_WINDOWS}/icx.exe`,
                `${CONDA_WINDOWS}/icx.exe`,
                "-DCOMPAT_SYCL=ON",
                "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
            ).slice(0, 1),
            ["cmake", "--build", "build", "--target", "compat_sycl"],
        ],
        files: SYCL_FILES,
    },
    {
        // nvcc compiles with cl on Windows, which the build finds in the
        // developer prompt and clice, outside it as under an editor, where
        // nvcc looks for it: in the newest toolset Visual Studio installed.
        // CCCL's headers take C++17 and cl's conforming preprocessor only.
        name: "cmake ninja nvcc msvc",
        platforms: ["win32"],
        msvc: true,
        requires: ["cmake", "ninja", `${CONDA_WINDOWS}/nvcc.exe`],
        build: [
            [
                "cmake",
                "-S",
                ".",
                "-B",
                "build",
                "-G",
                "Ninja",
                "-DCOMPAT_CUDA=ON",
                "-DCMAKE_CUDA_STANDARD=17",
                "-DCMAKE_CUDA_FLAGS=-Xcompiler=/Zc:preprocessor",
                `-DCMAKE_CUDA_COMPILER=${CONDA_WINDOWS}/nvcc.exe`,
                `-DCUDAToolkit_ROOT=${path.dirname(CONDA_WINDOWS)}`,
            ],
            ["cmake", "--build", "build", "--target", "compat_cuda"],
        ],
        files: {
            "cuda/kernel.cu": {
                recorded: [["-Xcompiler=/Zc:preprocessor"]],
                contains: [["-fcuda-is-device"]],
            },
        },
    },
];
