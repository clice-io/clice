/// Enforces the src/ include layering: core <- settings <- worker <-
/// project <- {server, analysis} <- driver, analysis reaching no worker;
/// inside core, support <- vfs <- command <- syntax <- compile <- semantic
/// <- index <- feature.
///
/// Each layer may include, or import the partitions of, downward only:
/// clice is one module, clice, whose partitions are named after the layer
/// they are in (`import :project.task_graph;`), and the test helpers after
/// `unittests` (`unittests.utils.tester`), which the check verifies too.
/// Bazel enforces the layering between its targets, as it checks that
/// every header a source includes belongs to the target or its
/// dependencies; the core directories share one target, so only this
/// check keeps them acyclic, as partition imports must be. It needs no
/// build.
///
/// Usage:
///   node tools/precheck/layers.ts

import * as fs from "node:fs";
import * as path from "node:path";
import { REPO_ROOT } from "../repo.ts";

const CORE = ["support", "vfs", "command", "syntax", "compile", "semantic", "index", "feature"];

/// By directory of src/, the prefixes its sources must never include.
const FORBIDDEN = new Map<string, readonly string[]>([
    ...CORE.map((layer, index): [string, string[]] => [
        layer,
        [
            ...CORE.slice(index + 1),
            "settings",
            "project",
            "worker",
            "server",
            "analysis",
            "driver",
        ].map((above) => `${above}/`),
    ]),
    ["settings", ["worker/", "project/", "server/", "analysis/", "driver/"]],
    ["worker", ["project/", "server/", "analysis/", "driver/"]],
    ["project", ["server/", "analysis/", "driver/"]],
    ["server", ["analysis/", "driver/"]],
    ["analysis", ["worker/", "server/", "driver/"]],
]);

const SOURCES = new Set([".h", ".cpp", ".cc", ".cppm"]);

const INCLUDE = /^\s*#include\s+"([^"]+)"/;
const IMPORT = /^\s*(?:export\s+)?import\s+:([\w.]+)\s*;/;
const PARTITION = /^\s*(?:export\s+)?module\s+clice:([\w.]+)\s*;/;

/// What a source of the src/ directory `layer` includes, or imports, from
/// a layer above it; a partition import names the path of its partition
/// (`import :project.task_graph;` is project/task_graph).
export function includeViolations(file: string, layer: string, text: string): string[] {
    const banned = FORBIDDEN.get(layer) ?? [];
    return text.split(/\r\n?|\n/).flatMap((line, index) => {
        const header = INCLUDE.exec(line)?.[1] ?? IMPORT.exec(line)?.[1]?.replaceAll(".", "/");
        if (header === undefined) {
            return [];
        }
        return banned
            .filter((prefix) => header.startsWith(prefix))
            .map(() => `${file}:${index + 1}: ${layer}/ must not include ${header}`);
    });
}

/// The partitions a module unit declares that are not named `<layer>.*`.
export function partitionViolations(file: string, layer: string, text: string): string[] {
    return text.split(/\r\n?|\n/).flatMap((line, index) => {
        const partition = PARTITION.exec(line)?.[1];
        return partition === undefined || partition.split(".")[0] === layer
            ? []
            : [`${file}:${index + 1}: the partition of this file is named ${layer}.*`];
    });
}

/// The files under a directory, sorted.
function filesUnder(directory: string): string[] {
    return fs
        .readdirSync(directory, { recursive: true, encoding: "utf8" })
        .sort()
        .map((file) => path.join(directory, file));
}

function violations(root: string): string[] {
    const src = path.join(root, "src");
    const read = (file: string): string => fs.readFileSync(file, "utf8");
    const found: string[] = [];
    for (const layer of FORBIDDEN.keys()) {
        for (const file of filesUnder(path.join(src, layer))) {
            if (SOURCES.has(path.extname(file))) {
                found.push(...includeViolations(path.relative(root, file), layer, read(file)));
            }
        }
    }
    const units = [
        ...filesUnder(src).map((file) => ({
            file,
            layer: path.relative(src, file).split(path.sep)[0] ?? "",
        })),
        ...filesUnder(path.join(root, "unittests")).map((file) => ({ file, layer: "unittests" })),
    ];
    for (const { file, layer } of units) {
        if (file.endsWith(".cppm")) {
            found.push(...partitionViolations(path.relative(root, file), layer, read(file)));
        }
    }
    return found;
}

function main(): void {
    const found = violations(REPO_ROOT);
    for (const violation of found) {
        console.log(violation);
    }
    if (found.length > 0) {
        console.log(
            `\n${found.length} layering violation(s): ` +
                "core <- settings <- worker <- project <- {server, analysis} <- driver, analysis " +
                "reaching no worker; inside core, " +
                CORE.join(" <- ") +
                "; includes and imports go downward only.",
        );
        process.exitCode = 1;
    }
}

if (import.meta.main) {
    main();
}
