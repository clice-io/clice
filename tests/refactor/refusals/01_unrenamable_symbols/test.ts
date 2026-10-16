import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --jobs 2");
    await t.cli("refactor rename CALL INVOKE");
    await t.cli("refactor rename print_line print --dry-run");
    await t.cli("refactor rename getter fetch --dry-run");
    await t.cli('refactor rename "Meters::operator+" plus --dry-run');
    await t.cli(
        `refactor rename ${t.place(t.at("main.cpp", "explicit |operator double"))} number --dry-run`,
    );
    await t.cli(
        `refactor rename ${t.place(t.at("main.cpp", 'Meters operator""|_m'))} meters --dry-run`,
    );
});
