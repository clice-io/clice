import { expect } from "vitest";
import { test } from "@clice/tools/testing";

/// The log's lines about the files' commands, each line's time, level and
/// source cut off; sorted, as the files open at once.
function commands(text: string): string[] {
    return text
        .split(/\r?\n/)
        .map((line) => line.replace(/^\[[^\]]*\] \[\w+\] (?:\[thread \d+\] )?\[[\w.-]+:\d+\] /, ""))
        .filter((message) => message.startsWith("Command of "))
        .sort();
}

// A folder nested in another, listed first: the lines still name each file
// by the folders' common directory, the outer folder's files as well.
test(async (t) => {
    const s = await t.start({ folders: ["app/sub", "app"] });
    s.open("app/main.cpp");
    s.open("app/sub/main.cpp");
    await s.sync();
    await s.stop();
    const logs = t.disk.list(".clice/logs/*/master.log");
    expect(logs).toHaveLength(1);
    await t.show("commands", commands(t.disk.read(logs[0] ?? "")));
});
