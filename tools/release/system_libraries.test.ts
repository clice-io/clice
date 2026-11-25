import * as path from "node:path";
import { describe, expect, test } from "vitest";
import {
    MALLOC_NOT_OURS,
    checkElf,
    checkMachO,
    checkPe,
    checkStaticElf,
    detectFormat,
    isDynamic,
    tidy,
} from "./system_libraries.ts";

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

    /// A Linux x64 build's `llvm-readelf --dyn-syms --wide`, cut short: mimalloc
    /// defines malloc and its kin.
    const symbols = `
Symbol table '.dynsym' contains 364 entries:
   Num:    Value          Size Type    Bind   Vis       Ndx Name
     0: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT   UND 
   300: 0000000000000000     0 FUNC    GLOBAL DEFAULT   UND fopen@GLIBC_2.2.5
   349: 0000000000e88040    74 FUNC    GLOBAL DEFAULT    14 malloc
   363: 0000000000e86e60   156 FUNC    GLOBAL DEFAULT    14 free
`;

    test("the shipped binary depends on glibc alone", () => {
        expect(checkElf(shipped, symbols)).toEqual({ violations: [], notes: [] });
    });

    test("malloc imported from glibc is a violation", () => {
        const imported = `
Symbol table '.dynsym' contains 75 entries:
   Num:    Value          Size Type    Bind   Vis       Ndx Name
    22: 0000000000000000     0 FUNC    GLOBAL DEFAULT   UND free@GLIBC_2.2.5
    36: 0000000000000000     0 FUNC    GLOBAL DEFAULT   UND malloc@GLIBC_2.2.5
`;
        expect(checkElf(shipped, imported).violations).toEqual([MALLOC_NOT_OURS]);
    });

    test("a C++ runtime or any other library is a violation", () => {
        const output = `${shipped}  0x0000000000000001 (NEEDED)       Shared library: [libstdc++.so.6]
  0x0000000000000001 (NEEDED)       Shared library: [libz.so.1]
`;
        expect(checkElf(output, symbols).violations).toEqual([
            "unexpected NEEDED dependency: libstdc++.so.6",
            "unexpected NEEDED dependency: libz.so.1",
        ]);
    });

    test("a conda RUNPATH or RPATH is noted, not failed", () => {
        const output = `${shipped}  0x000000000000001d (RUNPATH)      Library runpath: [/home/u/.pixi/envs/default/lib]
  0x000000000000000f (RPATH)        Library rpath: [/opt/conda/lib]
  0x000000000000001d (RUNPATH)      Library runpath: [$ORIGIN/../lib]
`;
        expect(checkElf(output, symbols)).toEqual({
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
        expect(checkElf(output, symbols).violations).toEqual([
            unparsed("NEEDED entries", "llvm-readelf -d"),
        ]);
    });
});

describe("checkStaticElf", () => {
    /// An x86_64-unknown-linux-musl build's `llvm-readelf --program-headers
    /// --wide`, cut short.
    const headers = `
Program Headers:
  Type           Offset   VirtAddr           PhysAddr           FileSiz  MemSiz   Flg Align
  PHDR           0x000040 0x0000000000200040 0x0000000000200040 0x0001f8 0x0001f8 R   0x8
  LOAD           0x000000 0x0000000000200000 0x0000000000200000 0xd22de8 0xd22de8 R   0x1000
  LOAD           0xd22e00 0x0000000000f23e00 0x0000000000f23e00 0x309d39d 0x309d39d R E 0x1000
  TLS            0x3dc01a0 0x0000000003fc21a0 0x0000000003fc21a0 0x000008 0x000158 R   0x8
  GNU_STACK      0x000000 0x0000000000000000 0x0000000000000000 0x000000 0x000000 RW  0x0
`;

    /// Its `llvm-readelf --syms --wide`, cut short: mimalloc's malloc is
    /// mi_malloc under another name.
    const symbols = `
Symbol table '.symtab' contains 114011 entries:
   Num:    Value          Size Type    Bind   Vis       Ndx Name
   283: 0000000000f25310    70 FUNC    LOCAL  HIDDEN      5 __libc_malloc
 81984: 0000000000f25310    70 FUNC    GLOBAL DEFAULT     5 mi_malloc
 82089: 0000000000f25310    70 FUNC    GLOBAL DEFAULT     5 malloc
`;

    test("a static program has no DYNAMIC segment, a dynamic one has", () => {
        expect(isDynamic(headers)).toBe(false);
        expect(
            isDynamic(`${headers}  DYNAMIC        0x419da78 0x000000000439da78 0x000000000439da78 0x000220 0x000220 RW  0x8
`),
        ).toBe(true);
    });

    test("the shipped program's malloc is mimalloc's", () => {
        expect(checkStaticElf(headers, symbols)).toEqual({ violations: [], notes: [] });
    });

    test("musl's malloc is a violation", () => {
        const musl = `
 81984: 0000000000f25310    70 FUNC    GLOBAL DEFAULT     5 mi_malloc
 82089: 0000000000f31a40   212 FUNC    GLOBAL DEFAULT     5 malloc
`;
        expect(checkStaticElf(headers, musl).violations).toEqual([MALLOC_NOT_OURS]);
    });

    test("a stripped program, or no segment read, fails closed", () => {
        expect(checkStaticElf(headers, "").violations).toEqual([
            "no malloc in the symbol table: a stripped program cannot tell whose malloc it is",
        ]);
        expect(checkStaticElf("", symbols).violations).toEqual([
            unparsed("LOAD segments", "llvm-readelf --program-headers"),
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
    /// An arm64 build's `llvm-nm -u`, cut short: mimalloc defines malloc and
    /// registers its zone.
    const imports = `_malloc_default_zone
_malloc_zone_register
_memcpy
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
        expect(checkMachO(shipped, commands(), imports)).toEqual({ violations: [], notes: [] });
    });

    test("malloc imported from libSystem is a violation", () => {
        const system = `_calloc
_free
_malloc
_malloc_default_zone
_realloc
`;
        expect(checkMachO(shipped, commands(), system).violations).toEqual([MALLOC_NOT_OURS]);
    });

    test("no mimalloc zone, or no imports read at all, is a violation", () => {
        expect(checkMachO(shipped, commands(), "_malloc_default_zone\n").violations).toEqual([
            MALLOC_NOT_OURS,
        ]);
        expect(checkMachO(shipped, commands(), "").violations).toEqual([MALLOC_NOT_OURS]);
    });

    test("a dylib outside the system or the SDK's libc++ is a violation", () => {
        const output = `${shipped}\t@rpath/libLLVM.dylib (compatibility version 1.0.0, current version 1.0.0)
\t/usr/lib/libc++.1.dylib (compatibility version 1.0.0, current version 1700.255.0)
\t/usr/lib/libc++abi.dylib (compatibility version 1.0.0, current version 1700.255.0)
`;
        expect(checkMachO(output, commands(), imports).violations).toEqual([
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
            imports,
        );
        expect(check.violations).toEqual([
            "LC_RPATH points into a conda/pixi env: /Users/u/.pixi/envs/default/lib",
            "LC_RPATH points into a conda/pixi env: /Users/u/Build Area/.pixi/envs/default/lib",
        ]);
    });

    test("no dependency under the header fails closed", () => {
        expect(checkMachO("/tmp/clice/bin/clice:\n", commands(), imports).violations).toEqual([
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
