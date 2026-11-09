import { test } from "@clice/tools/testing";

// core/str.h is in the database too, so the index keeps a manifest for it.
// An edit to it rebuilds the two sources that include it, not itself.
// util/fmt.h, which no file includes, still pastes util/fmt.tpp in: what
// the fragment names is util's.
test(async (t) => {
    await t.cli("index");
    await t.cli("analyze modules impact");
    await t.cli("analyze modules edge util core");
});
