import { test } from "@clice/tools/testing";

const MISSING =
    "Toolchain query failed: (Driver .*missing-cc not found or not executable|Cannot find driver: missing-cc)";
const UNRESOLVED =
    "(of the converted entries could not be resolved|The compiler could not be queried)";

// Names past ASCII in the database, its response file and the command
// line print as they are written.
test({ errors: [MISSING, UNRESOLVED] }, async (t) => {
    await t.cli(["command", "源码/主程序.cpp"]);
    await t.cli(["command", "源码/主程序.cpp", "--json"]);
    await t.cli(["command", "convert", "构建/compile_commands.json"]);
    await t.cli(["command", "convert", "--", "missing-cc", "-I头文件", "-c", "源码/主程序.cpp"]);
});
