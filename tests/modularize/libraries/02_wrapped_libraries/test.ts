import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

// A program over two libraries, beta including alpha, a standard library
// whose C++ headers include the C library's, and libc++'s module sources
// for it: std.cppm includes the headers `import std` stands for and
// std.compat exports fake_puts. The partition wraps alpha and beta, stands
// std for the standard library and keeps the C library headers.
const ARGS = "--scope app/** --scope third/** --std stdmod";

// One worker: which header provides fake_widen, declared both in
// <cwide.h> and in a fragment widen.h pastes, depends on the order the
// units are indexed in (analyze/dependencies/06_unit_order).
async function indexed(t: TestContext): Promise<void> {
    await t.cli("index --jobs 1");
}

/// A copy of a run's output directory, as another machine's run left it.
function copy(t: TestContext, from: string, to: string): void {
    for (const file of [...t.disk.list(`${from}/**`), `${from}/.modularize`]) {
        t.disk.write(`${to}${file.slice(from.length)}`, t.disk.read(file));
    }
}

test("library interfaces", async (t) => {
    await indexed(t);
    await t.cli(`analyze modules interface alpha ${ARGS} --partition partition.json`);
    await t.cli(`analyze modules interface beta ${ARGS} --partition partition.json`);
});

// main.cpp reaches <cio.h> only through <fakecstdio>, which `import std`
// empties, while direct.cpp includes it itself; third.cpp's own <cio.h>
// takes only the type of <cva.h>. fake_puts comes from std.compat,
// direct.cpp's <cwin.h> brings fake_word, and widen.h declares fake_widen
// itself, in a fragment it pastes.
test("C library kept headers", async (t) => {
    await indexed(t);
    await t.cli(`analyze modules interface libc ${ARGS} --partition partition.json`);
    await t.cli(`analyze modules interface std ${ARGS} --partition partition.json`);
});

test("writes the wrapping", async (t) => {
    await indexed(t);
    // Nothing a run did not write is touched.
    t.disk.write("wrap/custom.cppm", "export module custom;\n");
    await t.cli(`modularize ${ARGS} --partition partition.json --out wrap`);

    // Unchanged files keep their timestamps.
    const old = new Date("2001-01-01T00:00:00Z");
    t.disk.touch("wrap/alpha.cppm", old);
    await t.cli(`modularize ${ARGS} --partition partition.json --out wrap`);
    expect(t.disk.mtime("wrap/alpha.cppm")).toEqual(old);

    // The same units without a header emptied.
    await t.cli(`modularize ${ARGS} --partition partition.json --no-mirrors --out bare`);
    expect(t.disk.read("bare/beta.cppm")).toBe(t.disk.read("wrap/beta.cppm"));

    // Without beta, what the last run wrote for it goes; the rest stays.
    await t.cli(`modularize ${ARGS} --partition partitions/alpha.json --out wrap`);
});

// A second configuration, as its own run would have written it: alpha
// exports blue for Color, its alias names another namespace, and one more
// header is emptied.
function secondConfiguration(t: TestContext): void {
    copy(t, "wrap", "two");
    t.disk.edit(
        "two/alpha.cppm",
        { replace: "using enum ::alpha::Color;", with: "using ::alpha::blue;" },
        { replace: "export namespace al = ::alpha;", with: "export namespace al = ::alpha::wide;" },
    );
    t.disk.write(
        "two/alpha.macros.h",
        t.disk.read("wrap/alpha.macros.h") + "#define ALPHA_WIDE_ONLY 1\n",
    );
    t.disk.write("two/mirror/alpha/alpha/wide.h", "");
    t.disk.write("two/.modularize", t.disk.read("two/.modularize") + "mirror/alpha/alpha/wide.h\n");
}

test("merges configurations", async (t) => {
    await indexed(t);
    await t.cli(`modularize ${ARGS} --partition partition.json --out wrap`);
    secondConfiguration(t);
    await t.cli("modularize merge merges/two.json --out merged");
    await t.cli("modularize merge merges/two.json --no-mirrors --out bare");
    expect(t.disk.read("bare/alpha.cppm")).toBe(t.disk.read("merged/alpha.cppm"));

    // A third configuration like the second: what both export is under
    // either condition.
    copy(t, "two", "three");
    await t.cli("modularize merge merges/three.json --out merged");

    await t.cli("modularize merge merges/unconditioned.json --out merged");
    t.disk.write(
        "three/.modularize",
        t.disk.read("three/.modularize").replace(/^mirror\/std\/.*\n/gm, ""),
    );
    await t.cli("modularize merge merges/three.json --out merged");
    await t.cli("modularize merge merges/three.json --no-mirrors --out bare3");
    t.disk.write("two/.modularize", t.disk.read("two/.modularize").replace("beta.cppm\n", ""));
    await t.cli("modularize merge merges/two.json --out merged");
    await t.cli("modularize merge merges/mirror.json --out merged");
    await t.cli("modularize merge merges/two.json --partition partition.json --out merged");
    await t.cli("modularize merge merges/two.json");
});

test("partition errors", async (t) => {
    await indexed(t);
    for (const partition of [
        "external",
        "cycle",
        "named",
        "flags",
        "both",
        "rewritten",
        "wrapped",
        "primary",
    ]) {
        await t.cli(`modularize ${ARGS} --partition partitions/${partition}.json --out wrap`);
    }
    await t.cli("modularize --scope app/** --scope third/** --partition partition.json --out wrap");
    await t.cli("modularize --partition partition.json --std third/std --out wrap");
    await t.cli("modularize --out wrap");
    await t.cli(`modularize ${ARGS} --partition partition.json --out wrap --configuration nowhere`);
});
