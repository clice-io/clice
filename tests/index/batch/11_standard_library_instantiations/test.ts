import { test } from "@clice/tools/testing";

// std::call, of a system header, calls Task::go in its instantiation for
// Task: the index leaves that call out until a rule's index_function_body
// asks for it.
test("left out", async (t) => {
    await t.cli("index");
    await t.cli("query references Task::go");
});

test("asked for", async (t) => {
    t.disk.write("clice.toml", "[[rules]]\nindex_function_body = true\n");
    await t.cli("index");
    await t.cli("query references Task::go");
});
