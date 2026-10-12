import { test } from "@clice/tools/testing";

// The .clang-tidy both units read does not parse: the run reports it once
// and checks neither unit under the defaults instead.
test({ errors: ["Invalid .clang-tidy"] }, async (t) => {
    await t.cli("lint --workers 2");
});
