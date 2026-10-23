/// The macro oracle: what the real compiler defines under a database
/// entry's own flags, and which headers it finds, written as a header of
/// `#error` checks that the entry's source includes when it exists. The
/// real build ran before the header did, so only clice's parse sees it,
/// and every answer clice gets wrong is reported as a compile error naming
/// the macro or the header and what the compiler has.

import * as fs from "node:fs";
import * as path from "node:path";
import { cygpath, entryArguments, entrySource, samePath, type DatabaseEntry } from "./database.ts";
import { run } from "./scenario.ts";

/// Macros whose values follow from the compiler, its target and the
/// command's flags: language mode, target, data model, the semantic
/// switches clice keeps, and the versions of the C and C++ libraries the
/// headers come from. All are integers, so `#if` can compare them.
/// `__GNUC__` is left out on purpose: clice parses as clang, which reports
/// GCC 4.2 compatibility whichever GCC built the project.
const GNU_MACROS = [
    "__cplusplus",
    "__STDC_VERSION__",
    "__STDC_HOSTED__",
    "__STRICT_ANSI__",
    "__x86_64__",
    "__i386__",
    "__aarch64__",
    "__arm__",
    "__riscv",
    "__riscv_xlen",
    "__riscv_mul",
    "__riscv_compressed",
    "__riscv_zba",
    "__linux__",
    "_WIN32",
    "_WIN64",
    "__MINGW32__",
    "__MINGW64__",
    "__APPLE__",
    "__SIZEOF_INT__",
    "__SIZEOF_LONG__",
    "__SIZEOF_POINTER__",
    "__SIZEOF_WCHAR_T__",
    "__CHAR_UNSIGNED__",
    "__OPTIMIZE__",
    "__OPTIMIZE_SIZE__",
    "__NO_INLINE__",
    "__FAST_MATH__",
    "__EXCEPTIONS",
    "__GXX_RTTI",
    "__cpp_exceptions",
    "__cpp_rtti",
    "_REENTRANT",
    "_TIME_BITS",
    "_FILE_OFFSET_BITS",
    "__thumb__",
    "__SANITIZE_ADDRESS__",
    "__SSE4_2__",
    "__AVX2__",
    "__GLIBC__",
    "__GLIBC_MINOR__",
    "__NEWLIB__",
    "__NEWLIB_MINOR__",
    "__PICOLIBC__",
    "__PICOLIBC_MINOR__",
    "__BIONIC__",
    "__ANDROID_API__",
    "__MINGW64_VERSION_MAJOR",
    "__GLIBCXX__",
    "_GLIBCXX_RELEASE",
    "_LIBCPP_VERSION",
];

/// The same for cl and clang-cl, which spell target and runtime library
/// in their own macros. The language standard is `_MSVC_LANG`, the macro
/// code written for cl reads: cl keeps `__cplusplus` at 199711L without
/// /Zc:__cplusplus and leaves `__STDC_VERSION__` undefined without
/// /std:c11, while clice, like clang-cl, reports the standard it parses.
const MSVC_MACROS = [
    "_MSVC_LANG",
    "_MSC_VER",
    "_WIN32",
    "_WIN64",
    "_M_X64",
    "_M_ARM64",
    "_CPPRTTI",
    "_CPPUNWIND",
    "_MT",
    "_DLL",
    "_DEBUG",
    "_CHAR_UNSIGNED",
    "__AVX2__",
    "_MSVC_STL_VERSION",
];

/// Headers none of clice's own builtin headers is, so clice finds one
/// exactly where the toolchain has it: in GCC's private include directory
/// (`omp.h`, `quadmath.h`, `openacc.h`), or in another clang's resource
/// directory (Apple's `ptrcheck.h`, compiler-rt's). The sources include the
/// ones the compiler finds, so clice parses them too.
const HEADERS = [
    "omp.h",
    "quadmath.h",
    "openacc.h",
    "ptrcheck.h",
    "sanitizer/memprof_interface.h",
    "xray/xray_interface.h",
];

/// What nvcc defines for every pass. `__CUDA_ARCH__` stays out: the
/// preprocessing run below is nvcc's host pass, clice parses a CUDA source
/// in the device view.
const NVCC_MACROS = ["__NVCC__", "__CUDACC__", "__CUDACC_VER_MAJOR__", "__CUDACC_VER_MINOR__"];

/// Arguments that name the build's outputs or its own dependency files —
/// rerunning the entry must neither overwrite them nor compile. MSVC
/// options carry their values joined.
const DROPPED = new Set(["-c", "-MD", "-MMD", "-MP"]);
const DROPPED_WITH_VALUE = new Set(["-o", "-MF", "-MT", "-MQ"]);
const MSVC_DROPPED = /^[/-](c|FS|showIncludes|Fo.*|Fd.*)$/;

/// What the entry's compiler answers under the entry's flags.
export interface CompilerAnswers {
    /// Each macro's value, undefined for an undefined macro.
    macros: Map<string, string | undefined>;
    /// Whether it finds each of HEADERS.
    headers: Map<string, boolean>;
}

/// The entry rerun on a probe file of the same extension, preprocessing
/// only.
export async function compilerAnswers(
    entry: DatabaseEntry,
    scratch: string,
    env: NodeJS.ProcessEnv,
    posix?: string,
): Promise<CompilerAnswers> {
    const source = entrySource(entry);
    const [driver, ...rest] = entryArguments(entry);
    if (driver === undefined) {
        throw new Error(`${source}: empty compile command`);
    }
    const msvc = /^(cl|clang-cl)(\.exe)?$/i.test(path.basename(driver));
    const nvcc = /^nvcc(\.exe)?$/i.test(path.basename(driver));
    const macros = msvc ? MSVC_MACROS : nvcc ? NVCC_MACROS : GNU_MACROS;
    const probe = path.join(scratch, `probe${path.extname(source)}`);
    // The libraries define their versions in their headers: the probe reads
    // one, as the sources do before they include their expectations.
    const library = nvcc
        ? ""
        : path.extname(source) === ".c"
          ? "#include <string.h>\n"
          : "#include <cstdio>\n";
    const headers = HEADERS.map(
        (header) =>
            `#if __has_include(<${header}>)\n"<${header}>"=1\n#else\n"<${header}>"=0\n#endif\n`,
    );
    fs.writeFileSync(
        probe,
        library + macros.map((name) => `"${name}"=${name}\n`).join("") + headers.join(""),
    );

    // A POSIX database names the source as its runtime spells it.
    const written = posix === undefined ? [] : rest.filter((arg) => arg.startsWith("/"));
    const mapped = posix === undefined || written.length === 0 ? [] : await cygpath(posix, written);
    const native = new Map(written.map((arg, i) => [arg, mapped[i] ?? arg]));
    const args: string[] = [];
    for (let i = 0; i < rest.length; i += 1) {
        const arg = rest[i] ?? "";
        if (!msvc && DROPPED_WITH_VALUE.has(arg)) {
            i += 1;
        } else if (
            !(msvc ? MSVC_DROPPED.test(arg) : DROPPED.has(arg)) &&
            !samePath(path.resolve(entry.directory, native.get(arg) ?? arg), source)
        ) {
            args.push(arg);
        }
    }
    // The probe takes the source's place; options stay before a `--` that
    // ends them (CMake writes one before the source for clang-cl).
    const end = args.indexOf("--");
    const preprocess = msvc ? ["/EP"] : nvcc ? ["-E"] : ["-E", "-P"];
    args.splice(end === -1 ? args.length : end, 0, ...preprocess);
    args.push(probe);

    // The runtime's env starts the compiler as its shell did: a POSIX
    // path, without `.exe`, or a Cygwin symlink (/usr/bin/cc).
    const probed =
        posix === undefined
            ? await run(driver, args, { cwd: entry.directory, env })
            : await run(path.join(posix, "env.exe"), [driver, ...args], {
                  cwd: entry.directory,
                  env,
              });
    if (probed.status !== 0) {
        throw new Error(
            `${source}: preprocessing the probe with the entry's compiler failed ` +
                `(${probed.error ?? `exit ${probed.status}`}): ${driver} ${args.join(" ")}\n${probed.stderr}`,
        );
    }
    const answers: CompilerAnswers = { macros: new Map(), headers: new Map() };
    for (const line of probed.stdout.split("\n")) {
        const macro = /^"(\w+)"\s*=\s*(.*?)\s*$/.exec(line);
        if (macro?.[1] !== undefined && macro[2] !== undefined) {
            answers.macros.set(macro[1], macro[2] === macro[1] ? undefined : macro[2]);
        }
        const header = /^"<(.+)>"\s*=\s*([01])\s*$/.exec(line);
        if (header?.[1] !== undefined) {
            answers.headers.set(header[1], header[2] === "1");
        }
    }
    for (const name of [...macros, ...HEADERS]) {
        if (!answers.macros.has(name) && !answers.headers.has(name)) {
            throw new Error(`${source}: the probe output lacks ${name}:\n${probed.stdout}`);
        }
    }
    return answers;
}

/// Write the expectation header the source includes: `<file>.expect.h`
/// beside it.
export function writeExpectations(source: string, answers: CompilerAnswers): void {
    const lines: string[] = [];
    for (const [header, found] of answers.headers) {
        lines.push(
            `#if ${found ? "!" : ""}__has_include(<${header}>)`,
            `#error "<${header}>: the compiler ${found ? "finds" : "does not find"} it"`,
            "#endif",
        );
    }
    for (const [name, value] of answers.macros) {
        if (value === undefined) {
            lines.push(
                `#ifdef ${name}`,
                `#error "${name}: the compiler leaves it undefined"`,
                "#endif",
            );
        } else if (/^-?\d+[uUlL]*$/.test(value)) {
            lines.push(
                `#if !defined(${name}) || ${name} != ${value}`,
                `#error "${name}: the compiler has ${value}"`,
                "#endif",
            );
        } else {
            throw new Error(`${source}: ${name} is '${value}', not an integer #if can compare`);
        }
    }
    fs.writeFileSync(`${source}.expect.h`, `${lines.join("\n")}\n`);
}
