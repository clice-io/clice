/// Cross and embedded compilers, and the toolchains that set a target's
/// headers up themselves: the NDK, Emscripten, zig, xclang.

import * as path from "node:path";
import type { Scenario } from "@clice/tools/compat/scenario";
import { REPO_ROOT } from "@clice/tools/repo";
import { BOTH, cmake, cmakeNinja } from "./shared.ts";

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

export const SCENARIOS: Scenario[] = [
    cmakeNinja(
        "cmake ninja riscv64 cross",
        ["linux"],
        "/usr/bin/riscv64-linux-gnu-gcc",
        "/usr/bin/riscv64-linux-gnu-g++",
        { extra: ["-DCMAKE_SYSTEM_NAME=Linux", "-DCMAKE_SYSTEM_PROCESSOR=riscv64"] },
    ),
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
