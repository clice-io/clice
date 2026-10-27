/// The compatibility matrix. Each scenario names its compiler by absolute
/// path where the system has several, so the database records the
/// toolchain under test rather than whatever PATH finds first.

import * as path from "node:path";
import type { FileExpectation, Scenario } from "@clice/tools/compat/scenario";
import { REPO_ROOT } from "@clice/tools/repo";

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
    options: { msvc?: boolean; extra?: string[] } = {},
): Scenario {
    return {
        name,
        platforms,
        msvc: options.msvc === true,
        requires: ["cmake", "ninja", cc, cxx],
        build: cmake("Ninja", cc, cxx, ...(options.extra ?? [])),
        files: BOTH,
    };
}

/// Cross compilers for a Linux target build with the target's own libc.
function cmakeCross(name: string, prefix: string, processor: string, flags = ""): Scenario {
    return cmakeNinja(name, ["linux"], `/usr/bin/${prefix}-gcc`, `/usr/bin/${prefix}-g++`, {
        extra: [
            "-DCMAKE_SYSTEM_NAME=Linux",
            `-DCMAKE_SYSTEM_PROCESSOR=${processor}`,
            `-DCMAKE_C_FLAGS=${flags}`,
            `-DCMAKE_CXX_FLAGS=${flags}`,
        ],
    });
}

/// Conda's GCC and clang of the pixi `toolchains` environment, run without
/// its activation as a build outside it runs them.
const CONDA = path.join(REPO_ROOT, ".pixi", "envs", "toolchains", "bin");

/// xclang, the toolchain clice itself builds with: one clang for every
/// target, whose configuration file (bin/<triple>.cfg, read for
/// --target) names the target's sysroot, libc++ and runtimes.
const XCLANG = path.join(REPO_ROOT, ".pixi", "envs", "toolchains", "opt", "xclang", "bin");

function xclang(target: string): Scenario {
    const flag = `--target=${target}`;
    return {
        name: `make bear xclang ${target}`,
        platforms: ["linux"],
        requires: ["bear", "make", `${XCLANG}/clang`, `${XCLANG}/clang++`],
        build: [
            ["bear", "--", "make", `CC=${XCLANG}/clang ${flag}`, `CXX=${XCLANG}/clang++ ${flag}`],
        ],
        files: BOTH,
    };
}

/// zig's own targets, an architecture, an OS and a libc with its version,
/// whose headers zig sets up itself.
function zigTarget(target: string): Scenario {
    const flags = `-target ${target}`;
    return {
        name: `cmake ninja zig ${target}`,
        platforms: ["linux"],
        requires: ["cmake", "ninja", "zig"],
        build: cmake(
            "Ninja",
            "zig;cc",
            "zig;c++",
            `-DCMAKE_C_FLAGS=${flags}`,
            `-DCMAKE_CXX_FLAGS=${flags}`,
        ),
        files: BOTH,
    };
}

/// The NDK the runner images ship.
const NDK = process.env["ANDROID_NDK_LATEST_HOME"] ?? process.env["ANDROID_NDK_HOME"] ?? "";

const LLVM_WINDOWS = "C:/Program Files/LLVM/bin";
const MINGW = "C:/mingw64/bin";

/// MSYS2's UCRT GCC and its clang environment, whose clang targets
/// windows-gnu with its own libc++ and resource tree.
const MSYS2_UCRT = "C:/msys64/ucrt64/bin";
const MSYS2_CLANG = "C:/msys64/clang64/bin";

/// Cygwin and MSYS2's own tools, whose builds write POSIX paths: Cygwin's
/// CMake records `/usr/bin/cc`, a Cygwin symlink, for C.
const CYGWIN = "C:/cygwin64/bin";
const MSYS2 = "C:/msys64/usr/bin";

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
    cmakeNinja(
        "cmake ninja riscv64 cross",
        ["linux"],
        "/usr/bin/riscv64-linux-gnu-gcc",
        "/usr/bin/riscv64-linux-gnu-g++",
        { extra: ["-DCMAKE_SYSTEM_NAME=Linux", "-DCMAKE_SYSTEM_PROCESSOR=riscv64"] },
    ),
    // Conda's GCC compiles against its own sysroot's glibc.
    cmakeNinja(
        "cmake ninja conda gcc",
        ["linux"],
        `${CONDA}/x86_64-conda-linux-gnu-gcc`,
        `${CONDA}/x86_64-conda-linux-gnu-g++`,
    ),
    cmakeNinja("cmake ninja conda clang", ["linux"], `${CONDA}/clang`, `${CONDA}/clang++`),
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
    // GCC's two-core tuning form, a name clang does not know.
    cmakeCross(
        "cmake ninja aarch64 cross",
        "aarch64-linux-gnu",
        "aarch64",
        "-mcpu=cortex-a72.cortex-a53",
    ),
    // The distribution's defaults come from GCC's specs: Thumb, ARMv7 and
    // a 64-bit time_t.
    cmakeCross("cmake ninja armhf cross", "arm-linux-gnueabihf", "arm"),
    {
        name: "cmake ninja android ndk",
        platforms: ["linux"],
        requires: ["cmake", "ninja", `${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++`],
        build: [
            [
                "cmake",
                "-S",
                ".",
                "-B",
                "build",
                "-G",
                "Ninja",
                `-DCMAKE_TOOLCHAIN_FILE=${NDK}/build/cmake/android.toolchain.cmake`,
                "-DANDROID_ABI=arm64-v8a",
                "-DANDROID_PLATFORM=29",
            ],
            ["cmake", "--build", "build"],
        ],
        files: BOTH,
    },
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
    {
        name: "cmake ninja zig",
        platforms: ["linux"],
        requires: ["cmake", "ninja", "zig"],
        build: cmake("Ninja", "zig;cc", "zig;c++"),
        files: BOTH,
    },
    ...["x86_64-linux-musl", "aarch64-linux-gnu.2.28", "riscv64-linux-musl"].map(zigTarget),
    {
        name: "emcmake emscripten",
        platforms: ["linux"],
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
    // Its macOS and MSVC targets read Apple's and Microsoft's SDKs, which
    // xclang fetches only on a license accepted by hand.
    ...[
        "x86_64-unknown-linux-gnu",
        "aarch64-unknown-linux-gnu",
        "x86_64-w64-windows-gnu",
        "aarch64-w64-windows-gnu",
    ].map(xclang),
    {
        // newlib and its libstdc++, without any target flags.
        name: "make bear arm-none-eabi defaults",
        platforms: ["linux"],
        requires: ["bear", "make", "/usr/bin/arm-none-eabi-gcc", "/usr/bin/arm-none-eabi-g++"],
        build: [
            [
                "bear",
                "--",
                "make",
                "CC=/usr/bin/arm-none-eabi-gcc",
                "CXX=/usr/bin/arm-none-eabi-g++",
            ],
        ],
        files: BOTH,
    },
    {
        // musl's headers, which the wrapper's specs put in place of glibc's.
        name: "make bear musl-gcc",
        platforms: ["linux"],
        requires: ["bear", "make", "/usr/bin/musl-gcc"],
        build: [["bear", "--", "make", "build/util.o", "CC=/usr/bin/musl-gcc"]],
        files: { "src/util.c": {} },
    },
    {
        name: "make bear riscv bare metal",
        platforms: ["linux"],
        requires: ["bear", "make", "/usr/bin/riscv64-unknown-elf-gcc"],
        build: [
            [
                "bear",
                "--",
                "make",
                "build/util.o",
                "CC=/usr/bin/riscv64-unknown-elf-gcc",
                "CFLAGS=--specs=picolibc.specs -march=rv32imac_zba_zbb_zbc_zbs -mabi=ilp32 -mtune=sifive-7-series -msave-restore -Os -ffunction-sections -fdata-sections -fno-common",
            ],
        ],
        files: { "src/util.c": {} },
    },
    {
        // A build for WCH's RISC-V chips as MounRiver writes one, less WCH's
        // own `_xw` extension: clang knows -msmall-data-limit= as -G, which
        // RISC-V GCC rejects.
        name: "make bear riscv embedded",
        platforms: ["linux"],
        requires: ["bear", "make", "/usr/bin/riscv64-unknown-elf-gcc"],
        build: [
            [
                "bear",
                "--",
                "make",
                "build/util.o",
                "CC=/usr/bin/riscv64-unknown-elf-gcc",
                "CFLAGS=--specs=picolibc.specs -march=rv32imc_zba_zbb_zbc_zbs -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -std=gnu99",
            ],
        ],
        files: { "src/util.c": { recorded: [["-msmall-data-limit=8"]] } },
    },
];
