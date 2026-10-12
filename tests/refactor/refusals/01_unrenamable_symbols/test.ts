import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --workers 2");
    await t.cli("refactor rename --symbol CALL --to INVOKE");
    await t.cli("refactor rename --symbol print_line --to print --dry-run");
    await t.cli("refactor rename --symbol getter --to fetch --dry-run");
    await t.cli('refactor rename --symbol "Meters::operator+" --to plus --dry-run');
    await t.cli(
        `refactor rename --symbol ${t.place(t.at("main.cpp", "explicit |operator double"))} --to number --dry-run`,
    );
    await t.cli(
        `refactor rename --symbol ${t.place(t.at("main.cpp", 'Meters operator""|_m'))} --to meters --dry-run`,
    );
});
