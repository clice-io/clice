import { test } from "@clice/tools/testing";

// A clice.toml that does not parse stops every command working on the
// project, with exit 2 and the place of the error; one answering in JSON
// says so as its failure.
test("malformed", async (t) => {
    t.disk.write("clice.toml", "[project\nclang_tidy = true\n");
    for (const command of [
        "lint",
        "index",
        "scan",
        "command main.cpp",
        "format --check",
        "inspect document-symbols main.cpp",
        "query project-files",
    ]) {
        await t.cli(command);
    }
});

// A misspelled key warns, and the rest of the file applies: the rule,
// left without patterns, reaches every file.
test("unknown key", async (t) => {
    t.disk.write("clice.toml", '[[rules]]\npattrens = ["other.cpp"]\nappend = ["-DONLY_OTHER"]\n');
    await t.cli("lint");
});
