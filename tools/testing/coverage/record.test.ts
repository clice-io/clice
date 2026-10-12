import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { afterEach, beforeEach, describe, expect, test } from "vitest";
import { readRecords, recordConfig, recordInvocation, recordToml } from "./record.ts";

let dir = "";
beforeEach(() => {
    dir = fs.mkdtempSync(path.join(os.tmpdir(), "clice-coverage-"));
    process.env["CLICE_COVERAGE_DIR"] = dir;
});
afterEach(() => {
    delete process.env["CLICE_COVERAGE_DIR"];
    fs.rmSync(dir, { recursive: true });
});

const recorded = (surface: "method" | "invocation" | "config"): string[] =>
    [...(readRecords(dir).get(surface) ?? [])].sort();

describe("records", () => {
    test("invocations drop values", () => {
        recordInvocation(["analyze", "modules", "--view=edge", "--from", "core"]);
        recordInvocation(["inspect", "--configuration", "release", "hover", "/tmp/a.cpp"]);
        expect(recorded("invocation")).toEqual([
            "analyze modules --view --from",
            "inspect --configuration",
        ]);
    });

    test("config from options and toml", () => {
        recordConfig({
            project: { readonly: "on" },
            rules: [{ patterns: ["**"], append: ["-DX"] }],
        });
        recordToml(
            [
                'default_configuration = "a"',
                "",
                "[[rules]]",
                'patterns = ["src/**"]',
                "[hover]",
                "show_aka = false",
            ].join("\n"),
        );
        expect(recorded("config")).toEqual([
            "default_configuration",
            "hover.show_aka",
            "project.readonly",
            "rules.append",
            "rules.patterns",
        ]);
    });
});
