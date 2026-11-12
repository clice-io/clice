/// What an editor client's end-to-end test checks on a fixture of
/// tests/editors: the fixture's scenario.json, which every harness reads
/// (editors/nvim/tests/e2e.lua, editors/vscode/src/test/e2e.test.ts). Paths
/// are relative to the fixture, `/` between directories; every fixture
/// compiles clean.

import * as fs from "node:fs";
import * as path from "node:path";

/// Where a command comes from, as clice/currentContext says.
const SOURCES = ["database", "host", "default", "borrowed", "fallback"] as const;

export interface Scenario {
    /// The file the editor opens first.
    file: string;
    /// Where hover, definition and completion ask: the start of the first
    /// occurrence of this text in `file`.
    symbol: string;
    /// A symbol of the background index, as workspace symbols name it; the
    /// query is the name up to its first `(`.
    indexed: string;
    /// The file definition at `symbol` lands in.
    definition?: string;
    /// Where `file`'s command comes from (clice/currentContext), and the
    /// file it borrows from.
    command: { source: (typeof SOURCES)[number]; from?: string };
    /// A header's contexts: the host clice picks, another to switch to, and
    /// how many the listing holds.
    contexts?: { automatic: string; other: string; total: number };
    /// The file switching between source and header opens from `file`, and
    /// back; null when `file` has no counterpart.
    counterpart?: string | null;
    /// A refactoring: the file, the text the cursor goes to the start of,
    /// the code action kind, and text the edit adds.
    refactor?: { file: string; at: string; kind: string; adds: string };
    /// The build configurations of the rules: the one the server starts
    /// with, and another to switch to.
    configurations?: { active: string; switch: string };
}

function fail(where: string, message: string): never {
    throw new Error(`${where}: ${message}`);
}

function record(
    where: string,
    value: unknown,
    keys: Record<string, boolean>,
): Record<string, unknown> {
    if (typeof value !== "object" || value === null || Array.isArray(value)) {
        fail(where, "an object");
    }
    const object = value as Record<string, unknown>;
    for (const key of Object.keys(object)) {
        if (!Object.hasOwn(keys, key)) {
            fail(where, `unknown key '${key}'`);
        }
    }
    for (const [key, required] of Object.entries(keys)) {
        if (required && object[key] === undefined) {
            fail(where, `'${key}' is required`);
        }
    }
    return object;
}

function text(where: string, value: unknown): string {
    if (typeof value !== "string" || value === "") {
        fail(where, "a non-empty string");
    }
    return value;
}

/// The scenario.json of the fixture at `dir`, checked against its files:
/// each file it names exists, each text it looks for occurs there.
export function readScenario(dir: string): Scenario {
    const where = path.join(dir, "scenario.json");
    let raw: unknown;
    try {
        raw = JSON.parse(fs.readFileSync(where, "utf8"));
    } catch (error) {
        fail(where, (error as Error).message);
    }
    const scenario = record(where, raw, {
        file: true,
        symbol: true,
        indexed: true,
        definition: false,
        command: true,
        contexts: false,
        counterpart: false,
        refactor: false,
        configurations: false,
    });
    const file = (key: string, value: unknown): string => {
        const name = text(`${where}: ${key}`, value);
        if (!fs.statSync(path.join(dir, name), { throwIfNoEntry: false })?.isFile()) {
            fail(where, `${key}: no file ${name} in the fixture`);
        }
        return name;
    };
    const occurs = (key: string, value: unknown, inFile: string): void => {
        const wanted = text(`${where}: ${key}`, value);
        if (!fs.readFileSync(path.join(dir, inFile), "utf8").includes(wanted)) {
            fail(where, `${key}: ${JSON.stringify(wanted)} is not in ${inFile}`);
        }
    };
    const opened = file("file", scenario["file"]);
    occurs("symbol", scenario["symbol"], opened);
    text(`${where}: indexed`, scenario["indexed"]);
    if (scenario["definition"] !== undefined) {
        file("definition", scenario["definition"]);
    }
    const command = record(`${where}: command`, scenario["command"], { source: true, from: false });
    if (!(SOURCES as readonly unknown[]).includes(command["source"])) {
        fail(where, `command.source: ${JSON.stringify(command["source"])} is no command source`);
    }
    if (command["from"] !== undefined) {
        file("command.from", command["from"]);
    }
    if (scenario["contexts"] !== undefined) {
        const contexts = record(`${where}: contexts`, scenario["contexts"], {
            automatic: true,
            other: true,
            total: true,
        });
        file("contexts.automatic", contexts["automatic"]);
        file("contexts.other", contexts["other"]);
        if (!Number.isInteger(contexts["total"]) || (contexts["total"] as number) < 2) {
            fail(where, "contexts.total: the number of contexts, two or more");
        }
    }
    if (scenario["counterpart"] !== undefined && scenario["counterpart"] !== null) {
        file("counterpart", scenario["counterpart"]);
    }
    if (scenario["refactor"] !== undefined) {
        const refactor = record(`${where}: refactor`, scenario["refactor"], {
            file: true,
            at: true,
            kind: true,
            adds: true,
        });
        occurs("refactor.at", refactor["at"], file("refactor.file", refactor["file"]));
        text(`${where}: refactor.kind`, refactor["kind"]);
        text(`${where}: refactor.adds`, refactor["adds"]);
    }
    if (scenario["configurations"] !== undefined) {
        const configurations = record(`${where}: configurations`, scenario["configurations"], {
            active: true,
            switch: true,
        });
        text(`${where}: configurations.active`, configurations["active"]);
        text(`${where}: configurations.switch`, configurations["switch"]);
    }
    return scenario as unknown as Scenario;
}
