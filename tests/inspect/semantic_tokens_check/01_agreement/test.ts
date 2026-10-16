import { test } from "@clice/tools/testing";

// `clice inspect semantic-tokens --check` sets the tokens the index projects
// beside the compiled file's: over these forms they agree token for token.
// What only the compile knows (inactive code, declarators, dependent names,
// format strings) is left out of the comparison; a write only an
// instantiation makes (`bump(value)` binding `int&` in `touch<int>`) is in
// neither.
test(async (t) => {
    await t.cli(["inspect", "semantic-tokens", "main.cpp", "--check"]);
});
