/// The macro oracle: what the real compiler defines under a database
/// entry's own flags, written as a header of `#error` checks that the
/// entry's source includes when it exists. The real build ran before the
/// header did, so only clice's parse sees it, and every value clice gets
/// wrong surfaces as a compile error naming the macro and the value the
/// compiler has.

import * as fs from "node:fs";
import * as path from "node:path";
import { entryArguments, entrySource, samePath, type DatabaseEntry } from "./database.ts";
import { run } from "./scenario.ts";

/// Macros whose values follow from the compiler, its target and the
/// command's flags: language mode, target, data model, and the semantic
/// switches clice keeps. All are integers, so `#if` can compare them.
/// `__GNUC__` is left out on purpose: clice parses as clang, which claims
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
    "__SANITIZE_ADDRESS__",
    "__SSE4_2__",
    "__AVX2__",
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

/// The macro values the entry's compiler has under the entry's flags:
/// the entry rerun on a probe file of the same extension, preprocessing
/// only. An undefined macro maps to undefined.
export async function compilerMacros(
    entry: DatabaseEntry,
    scratch: string,
    env: NodeJS.ProcessEnv,
): Promise<Map<string, string | undefined>> {
    const source = entrySource(entry);
    const [driver, ...rest] = entryArguments(entry);
    if (driver === undefined) {
        throw new Error(`${source}: empty compile command`);
    }
    const msvc = /^(cl|clang-cl)(\.exe)?$/i.test(path.basename(driver));
    const nvcc = /^nvcc(\.exe)?$/i.test(path.basename(driver));
    const macros = msvc ? MSVC_MACROS : nvcc ? NVCC_MACROS : GNU_MACROS;
    const probe = path.join(scratch, `probe${path.extname(source)}`);
    fs.writeFileSync(probe, macros.map((name) => `"${name}"=${name}\n`).join(""));

    const args: string[] = [];
    for (let i = 0; i < rest.length; i += 1) {
        const arg = rest[i] ?? "";
        if (!msvc && DROPPED_WITH_VALUE.has(arg)) {
            i += 1;
        } else if (
            !(msvc ? MSVC_DROPPED.test(arg) : DROPPED.has(arg)) &&
            !samePath(path.resolve(entry.directory, arg), source)
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

    const probed = await run(driver, args, { cwd: entry.directory, env });
    if (probed.status !== 0) {
        throw new Error(
            `${source}: preprocessing the probe with the entry's compiler failed ` +
                `(${probed.error ?? `exit ${probed.status}`}): ${driver} ${args.join(" ")}\n${probed.stderr}`,
        );
    }
    const values = new Map<string, string | undefined>();
    for (const line of probed.stdout.split("\n")) {
        const match = /^"(\w+)"\s*=\s*(.*?)\s*$/.exec(line);
        if (match?.[1] !== undefined && match[2] !== undefined) {
            values.set(match[1], match[2] === match[1] ? undefined : match[2]);
        }
    }
    for (const name of macros) {
        if (!values.has(name)) {
            throw new Error(`${source}: the probe output lacks ${name}:\n${probed.stdout}`);
        }
    }
    return values;
}

/// Write the expectation header the source includes: `<file>.expect.h`
/// beside it.
export function writeExpectations(source: string, values: Map<string, string | undefined>): void {
    const lines: string[] = [];
    for (const [name, value] of values) {
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
