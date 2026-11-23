import * as path from "node:path";
import { describe, expect, test } from "vitest";
import { checkElf, checkMachO, checkPe, detectFormat, tidy } from "./system_libraries.ts";

const bytes = (hex: string) => Buffer.from(hex, "hex");

/// The parse failure of each tool, when its output has no dependency.
const unparsed = (what: string, tool: string) =>
    `parsed no ${what} from \`${tool}\` output; refusing to vouch for this ` +
    `binary (the tool's output format may have changed)`;

describe("detectFormat", () => {
    test("reads ELF, every Mach-O magic and PE", () => {
        expect(detectFormat(bytes("7f454c46"))).toBe("elf");
        for (const magic of ["cffaedfe", "cefaedfe", "feedfacf", "feedface", "cafebabe"]) {
            expect(detectFormat(bytes(magic)), magic).toBe("macho");
        }
        expect(detectFormat(bytes("4d5a9000"))).toBe("pe");
        expect(detectFormat(bytes("4d5a"))).toBe("pe");
    });

    test("anything else is no object file", () => {
        expect(detectFormat(bytes("0a5b3230"))).toBeUndefined();
        expect(detectFormat(bytes("7f454c"))).toBeUndefined();
        expect(detectFormat(bytes(""))).toBeUndefined();
    });
});

describe("checkElf", () => {
    /// The v1.0.0 x86_64-unknown-linux-gnu package's `llvm-readelf -d`, cut short.
    const shipped = `
Dynamic section at offset 0x419da78 contains 31 entries:
  Tag                Type           Name/Value
  0x0000000000000001 (NEEDED)       Shared library: [libdl.so.2]
  0x0000000000000001 (NEEDED)       Shared library: [librt.so.1]
  0x0000000000000001 (NEEDED)       Shared library: [libpthread.so.0]
  0x0000000000000001 (NEEDED)       Shared library: [libm.so.6]
  0x0000000000000001 (NEEDED)       Shared library: [libc.so.6]
  0x0000000000000001 (NEEDED)       Shared library: [ld-linux-x86-64.so.2]
  0x000000006ffffffb (FLAGS_1)      PIE
  0x0000000000000015 (DEBUG)        0x0
`;

    test("the shipped binary depends on glibc alone", () => {
        expect(checkElf(shipped)).toEqual({ violations: [], notes: [] });
    });

    test("a C++ runtime or any other library is a violation", () => {
        const output = `${shipped}  0x0000000000000001 (NEEDED)       Shared library: [libstdc++.so.6]
  0x0000000000000001 (NEEDED)       Shared library: [libz.so.1]
`;
        expect(checkElf(output).violations).toEqual([
            "unexpected NEEDED dependency: libstdc++.so.6",
            "unexpected NEEDED dependency: libz.so.1",
        ]);
    });

    test("a conda RUNPATH or RPATH is noted, not failed", () => {
        const output = `${shipped}  0x000000000000001d (RUNPATH)      Library runpath: [/home/u/.pixi/envs/default/lib]
  0x000000000000000f (RPATH)        Library rpath: [/opt/conda/lib]
  0x000000000000001d (RUNPATH)      Library runpath: [$ORIGIN/../lib]
`;
        expect(checkElf(output)).toEqual({
            violations: [],
            notes: [
                "RUNPATH references a conda/pixi env: /home/u/.pixi/envs/default/lib",
                "RUNPATH references a conda/pixi env: /opt/conda/lib",
            ],
        });
    });

    test("no NEEDED entry fails closed", () => {
        const output = `  0x0000000000000001 (NEEDED)       0x1234
  0x0000000000000015 (DEBUG)        0x0
`;
        expect(checkElf(output).violations).toEqual([
            unparsed("NEEDED entries", "llvm-readelf -d"),
        ]);
    });
});

describe("checkMachO", () => {
    /// The v1.0.0 x86_64-apple-darwin package's `llvm-otool -L`.
    const shipped = `/tmp/clice/bin/clice:
\t/usr/lib/libxcselect.dylib (compatibility version 1.0.0, current version 1.0.0)
\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1356.0.0)
\t/usr/lib/libz.1.dylib (compatibility version 1.0.0, current version 1.2.12)
`;
    const commands = (...rpaths: string[]) =>
        [
            "Load command 12",
            "          cmd LC_LOAD_DYLIB",
            "      cmdsize 56",
            "         name /usr/lib/libSystem.B.dylib (offset 24)",
            ...rpaths.flatMap((rpath, index) => [
                `Load command ${String(13 + index)}`,
                "          cmd LC_RPATH",
                "      cmdsize 32",
                `         path ${rpath} (offset 12)`,
            ]),
        ].join("\n");

    test("the shipped binary depends on system dylibs alone", () => {
        expect(checkMachO(shipped, commands())).toEqual({ violations: [], notes: [] });
    });

    test("a dylib outside the system or the SDK's libc++ is a violation", () => {
        const output = `${shipped}\t@rpath/libLLVM.dylib (compatibility version 1.0.0, current version 1.0.0)
\t/usr/lib/libc++.1.dylib (compatibility version 1.0.0, current version 1700.255.0)
\t/usr/lib/libc++abi.dylib (compatibility version 1.0.0, current version 1700.255.0)
`;
        expect(checkMachO(output, commands()).violations).toEqual([
            "non-system dylib dependency: @rpath/libLLVM.dylib",
            "C++ runtime dylib dependency: /usr/lib/libc++.1.dylib",
            "C++ runtime dylib dependency: /usr/lib/libc++abi.dylib",
        ]);
    });

    test("an LC_RPATH into a conda env is a violation", () => {
        const check = checkMachO(
            shipped,
            commands(
                "@loader_path/../lib",
                "/Users/u/.pixi/envs/default/lib",
                "/Users/u/Build Area/.pixi/envs/default/lib",
            ),
        );
        expect(check.violations).toEqual([
            "LC_RPATH points into a conda/pixi env: /Users/u/.pixi/envs/default/lib",
            "LC_RPATH points into a conda/pixi env: /Users/u/Build Area/.pixi/envs/default/lib",
        ]);
    });

    test("no dependency under the header fails closed", () => {
        expect(checkMachO("/tmp/clice/bin/clice:\n", commands()).violations).toEqual([
            unparsed("dylib dependencies", "llvm-otool -L"),
        ]);
    });
});

describe("checkPe", () => {
    /// The v1.0.0 x86_64-w64-mingw32 package's `llvm-readobj --coff-imports`,
    /// cut short.
    const shipped = `
File: clice.exe
Format: COFF-x86-64
Arch: x86_64
AddressSize: 64bit
Import {
  Name: KERNEL32.dll
  ImportLookupTableRVA: 0x3D99600
  ImportAddressTableRVA: 0x3D9A360
  Symbol: CloseHandle (0)
}
Import {
  Name: api-ms-win-crt-stdio-l1-1-0.dll
  ImportLookupTableRVA: 0x3D99C58
  ImportAddressTableRVA: 0x3D9A9B8
}
`;

    test("the shipped binary imports system DLLs alone", () => {
        expect(checkPe(shipped)).toEqual({ violations: [], notes: [] });
    });

    test("a toolchain's runtime DLL is a violation, whatever its case", () => {
        const output = `${shipped}Import {
  Name: libc++.dll
}
Import {
  Name: LIBWINPTHREAD-1.DLL
}
Import {
  Name: VCRUNTIME140.dll
}
Import {
  Name: msvcp140.dll
}
Import {
  Name: msvcrt.dll
}
`;
        expect(checkPe(output).violations).toEqual([
            "runtime DLL dependency: libc++.dll",
            "runtime DLL dependency: LIBWINPTHREAD-1.DLL",
            "runtime DLL dependency: VCRUNTIME140.dll",
            "runtime DLL dependency: msvcp140.dll",
        ]);
    });

    test("no imported DLL fails closed", () => {
        expect(checkPe("File: clice.exe\nFormat: COFF-x86-64\n").violations).toEqual([
            unparsed("imported DLLs", "llvm-readobj --coff-imports"),
        ]);
    });
});

describe("tidy", () => {
    test("drops empty and `.` parts and keeps `..`", () => {
        expect(tidy("build/./bin//clice.stripped", path.posix)).toBe("build/bin/clice.stripped");
        expect(tidy("./clice/", path.posix)).toBe("clice");
        expect(tidy("/a/../b", path.posix)).toBe("/a/../b");
        expect(tidy("", path.posix)).toBe(".");
    });

    test("writes Windows paths with backslashes", () => {
        expect(tidy("build/RelWithDebInfo/bin/bin/clice.stripped", path.win32)).toBe(
            "build\\RelWithDebInfo\\bin\\bin\\clice.stripped",
        );
        expect(tidy("C:/x/./y/", path.win32)).toBe("C:\\x\\y");
        expect(tidy("C:\\", path.win32)).toBe("C:\\");
        expect(tidy("\\\\server\\share\\x\\.\\y", path.win32)).toBe("\\\\server\\share\\x\\y");
    });
});
