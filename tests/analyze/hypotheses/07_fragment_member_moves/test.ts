import { test } from "@clice/tools/testing";

// Clock::size is defined in util/clock.tpp, the fragment util/clock.h pastes
// in. Moving Clock to core takes its member's definition along, and with it
// util's only uses of core.
test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules edge util core");
    await t.cli("analyze modules edge util core --move-entity Clock=core/clock.h");
});
