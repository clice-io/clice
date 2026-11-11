import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --jobs 2");
    await t.cli("refactor rename CONSOLE_WIDTH WIDTH");
    await t.cli("refactor rename print_line print --dry-run");
    await t.cli("refactor rename getter fetch --dry-run");
    await t.cli('refactor rename "Meters::operator+" plus --dry-run');
    await t.cli(
        `refactor rename ${t.place(t.at("main.cpp", "explicit |operator double"))} number --dry-run`,
    );
    await t.cli(
        `refactor rename ${t.place(t.at("main.cpp", 'Meters operator""|_m'))} meters --dry-run`,
    );
    // Its instantiations resolve the name to two fields.
    await t.cli(`refactor rename ${t.place(t.at("main.cpp", "item.|value"))} amount --dry-run`);
    // The compile command defines it: no file holds a definition to change.
    await t.cli("refactor rename BUILD_LEVEL LEVEL --dry-run");
    await t.cli(`refactor rename ${t.place(t.at("main.cpp", "= |BUILD_LEVEL"))} LEVEL --dry-run`);
});
