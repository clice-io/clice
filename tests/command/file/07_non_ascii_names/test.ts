import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Driver .*missing-cc not found or not executable";
const NOT_FOUND = "Toolchain query failed: Cannot find driver: missing-cc";
const UNCONVERTED_ENTRIES = "of the entries stay driver commands";
const UNCONVERTED = "The command stays a driver command";

// Names past ASCII in the database, its response file and the command
// line print as they are written.
test({ errors: [MISSING, NOT_FOUND, UNCONVERTED_ENTRIES, UNCONVERTED] }, async (t) => {
    await t.cli(["command", "源码/主程序.cpp"]);
    await t.cli(["command", "源码/主程序.cpp", "--json"]);
    await t.cli(["command", "convert", "构建/compile_commands.json"]);
    await t.cli(["command", "convert", "--", "missing-cc", "-I头文件", "-c", "源码/主程序.cpp"]);
});
