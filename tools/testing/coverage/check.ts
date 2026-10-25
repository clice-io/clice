/// The coverage check: every item of the product surface against what the
/// suites recorded. An item is `method <name>`, `command <words>`,
/// `flag <command> --<name>` (`flag * --<name>` for one every command
/// takes) or `config <key path>`; an exemption names an item and says why.

import * as fs from "node:fs";
import { parse } from "smol-toml";
import type * as proto from "vscode-languageserver-protocol";
import { CliceClient } from "../../client/client.ts";
import { Workspace } from "../../client/workspace.ts";
import { serverEnv } from "../../process_gate.ts";
import * as protocol from "../../protocol/protocol.ts";
import { readRecords } from "./record.ts";
import { commandsAndFlags, configKeys, methods } from "./surface.ts";

/// The public clice requests a client may send; `clice/internal/*` are
/// test hooks, not surface.
function extensionMethods(): string[] {
    return Object.values(protocol)
        .map((value) => (value as { method?: unknown }).method)
        .filter((method): method is string => typeof method === "string")
        .filter((method) => method.startsWith("clice/") && !method.startsWith("clice/internal/"));
}

async function capabilities(clice: string): Promise<proto.ServerCapabilities> {
    const workspace = Workspace.tmp();
    const client = CliceClient.start(clice, { env: serverEnv() as Record<string, string> });
    try {
        await client.initialize(workspace);
        return client.initResult?.capabilities ?? {};
    } finally {
        await client.shutdown();
        fs.rmSync(workspace.root, { recursive: true, force: true });
    }
}

export async function surface(clice: string): Promise<string[]> {
    const { commands, flags } = commandsAndFlags(clice);
    const byFlag = new Map<string, string[]>();
    for (const entry of flags) {
        const at = entry.lastIndexOf(" --");
        const flag = entry.slice(at + 1);
        byFlag.set(flag, [...(byFlag.get(flag) ?? []), entry.slice(0, at)]);
    }
    const flagItems: string[] = [];
    for (const [flag, owners] of byFlag) {
        if (owners.length === commands.length) {
            flagItems.push(`flag * ${flag}`);
            continue;
        }
        // A flag every operation of a subcommand takes is the subcommand's.
        const bySubcommand = new Map<string, string[]>();
        for (const owner of owners) {
            const subcommand = owner.split(" ")[0] ?? owner;
            bySubcommand.set(subcommand, [...(bySubcommand.get(subcommand) ?? []), owner]);
        }
        for (const [subcommand, held] of bySubcommand) {
            const operations = commands.filter(
                (command) => command === subcommand || command.startsWith(`${subcommand} `),
            );
            if (held.length === operations.length) {
                flagItems.push(`flag ${subcommand} ${flag}`);
            } else {
                flagItems.push(...held.map((owner) => `flag ${owner} ${flag}`));
            }
        }
    }
    return [
        ...methods(await capabilities(clice), extensionMethods()).map(
            (method) => `method ${method}`,
        ),
        ...commands.map((command) => `command ${command}`),
        ...flagItems,
        ...configKeys(clice).map((key) => `config ${key}`),
    ].sort();
}

function startsWith(words: string[], prefix: string[]): boolean {
    return prefix.every((word, index) => words[index] === word);
}

function covered(item: string, records: ReturnType<typeof readRecords>): boolean {
    const space = item.indexOf(" ");
    const kind = item.slice(0, space);
    const name = item.slice(space + 1);
    const invocations = [...(records.get("invocation") ?? [])].map((line) => line.split(" "));
    switch (kind) {
        case "method":
            return records.get("method")?.has(name) ?? false;
        case "command":
            return invocations.some((words) => startsWith(words, name.split(" ")));
        case "flag": {
            const words = name.split(" ");
            const flag = words.pop() ?? "";
            const command = words[0] === "*" ? [] : words;
            return invocations.some((line) => startsWith(line, command) && line.includes(flag));
        }
        case "config":
            return [...(records.get("config") ?? [])].some(
                (key) => key === name || key.startsWith(`${name}.`),
            );
        default:
            throw new Error(`unknown surface item: ${item}`);
    }
}

/// What fails the check: items nothing exercised and nothing exempts, and
/// exemptions that no longer name an uncovered item of the surface.
export async function check(
    clice: string,
    recordsDir: string,
    exemptFile: string,
): Promise<string[]> {
    const exempt = parse(fs.readFileSync(exemptFile, "utf8")) as Record<string, string>;
    const records = readRecords(recordsDir);
    const items = await surface(clice);
    const out: string[] = [];
    for (const item of items) {
        if (!covered(item, records) && !(item in exempt)) {
            out.push(`uncovered: ${item}`);
        }
    }
    for (const item of Object.keys(exempt)) {
        if (!items.includes(item)) {
            out.push(`exempt but not surface: ${item}`);
        } else if (covered(item, records)) {
            out.push(`exempt but covered: ${item}`);
        }
    }
    return out;
}
