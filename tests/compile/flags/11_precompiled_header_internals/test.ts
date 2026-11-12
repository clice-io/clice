import { test } from "@clice/tools/testing";

// The units load the target's PCH, whose header declares names of internal
// linkage and a template. The PCH's builder writes the header's entries;
// the units write none there, yet use those names, a header function's
// own class among them, and instantiate the template with types of their
// own, internal and function-local.

test("the units index", async (t) => {
    await t.cli("index");
});

test("internal names across the pch", async (t) => {
    await t.cli("index");
    // `helper` and `scale` from a use, from the header, and their
    // definition from another unit.
    await t.cli(
        `query references ${t.place(t.at("src/main.cpp", "scale * |helper"))} --include-declaration`,
    );
    await t.cli(
        `query references ${t.place(t.at("include/shapes.h", "static double |helper"))} --include-declaration`,
    );
    await t.cli(`query definition ${t.place(t.at("src/other.cpp", "return |helper"))}`);
    await t.cli(
        `query references ${t.place(t.at("src/main.cpp", "return |scale"))} --include-declaration`,
    );
    await t.cli(`query definition ${t.place(t.at("src/other.cpp", "helper(|scale)"))}`);
    // The members the header's template reads, from their declarations and
    // from the read.
    await t.cli(
        `query references ${t.place(t.at("src/local.cpp", "double |radius;"))} --include-declaration`,
    );
    await t.cli(
        `query references ${t.place(t.at("src/local.cpp", "double |radius = 0"))} --include-declaration`,
    );
    await t.cli(
        `query references ${t.place(t.at("include/shapes.h", "shape.|radius"))} --include-declaration`,
    );
    // A header function's own class, reached through `auto`.
    await t.cli(`query definition ${t.place(t.at("src/made.cpp", "make().|value"))}`);
    await t.cli(
        `query references ${t.place(t.at("include/shapes.h", "double |value;"))} --include-declaration`,
    );
});

test("the server reads them alike", async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("helper", s.references(t.at("include/shapes.h", "static double |helper")));
    await t.show("scale", s.references(t.at("src/other.cpp", "helper(|scale)")));
    await t.show("local", s.references(t.at("src/local.cpp", "double |radius;")));
    await t.show("scoped", s.references(t.at("src/local.cpp", "double |radius = 0")));
});
