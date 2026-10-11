/// The docs that documented cases make: a feature page's sections, a
/// command page's examples, the configuration page's option examples.
/// `regionItems` fills a page's generated region for the docs site, which
/// draws each ```case fence as a card; `previewPages` writes standalone
/// markdown with the sources and outputs inline, to read without the site.

import * as fs from "node:fs";
import * as path from "node:path";
import { parseAnnotations } from "../../snap/annotation.ts";
import type { Case } from "../cases/case.ts";
import { SNAPSHOT, statusOf } from "../cases/output.ts";
import { CASES_DIR, MARKED, copyName } from "../cases/project.ts";
import { TOPICS } from "../cases/topics.ts";

function documented(cases: readonly Case[]): Case[] {
    return cases.filter((c) => c.readme !== undefined);
}

/// How much of the case's output is right, by its known-wrong flags.
function status(c: Case): string {
    const file = path.join(c.dir, SNAPSHOT);
    return statusOf(fs.existsSync(file) ? fs.readFileSync(file, "utf8") : "");
}

function heading(name: string): string {
    const words = name.split("_").join(" ");
    return words.charAt(0).toUpperCase() + words.slice(1);
}

function language(file: string): string {
    if (MARKED.test(file)) {
        return "cpp";
    }
    if (file.endsWith(".toml")) {
        return "toml";
    }
    if (file.endsWith(".json")) {
        return "json";
    }
    return "text";
}

/// A documented case as a page item: its title, status and prose.
function item(c: Case, body: readonly string[]): string[] {
    const readme = c.readme;
    if (readme === undefined) {
        return [];
    }
    const issues = readme.issues.length === 0 ? "" : ` (${readme.issues.join(", ")})`;
    return [
        `### ${readme.title}`,
        "",
        `*${status(c)}${issues}*`,
        "",
        readme.summary,
        "",
        ...(readme.prose === "" ? [] : [readme.prose, ""]),
        ...body,
    ];
}

/// The generated region of section `subtopic` of a page: one card per
/// documented case, in case order, in the docs site's card frame (status
/// and issues in the opening comment, the title the first paragraph).
export function regionItems(cases: readonly Case[], topic: string, subtopic: string): string {
    return documented(cases)
        .filter((c) => c.topic === topic && c.subtopic === subtopic)
        .flatMap((c) => {
            const readme = c.readme;
            if (readme === undefined) {
                return [];
            }
            return [
                `<!-- BEGIN CAPABILITY: ${[status(c), ...readme.issues].join(" ")} -->`,
                "",
                `**${readme.title}**`,
                "",
                readme.summary,
                "",
                ...(readme.prose === "" ? [] : [readme.prose, ""]),
                "```case",
                `${path.basename(CASES_DIR)}/${c.id}`,
                "```",
                "",
                "<!-- END CAPABILITY -->",
                "",
            ];
        })
        .join("\n")
        .trimEnd();
}

/// A fence longer than any run of backticks in `text`.
function fence(text: string, info: string): string[] {
    const longest = Math.max(2, ...[...text.matchAll(/`+/g)].map((match) => match[0].length));
    const ticks = "`".repeat(longest + 1);
    return [`${ticks}${info}`, text, ticks];
}

function inline(c: Case, files: "all" | "config"): string[] {
    const out: string[] = [];
    for (const rel of c.inputs) {
        const name = copyName(rel);
        if (files === "config" && name !== "clice.toml") {
            continue;
        }
        const raw = fs.readFileSync(path.join(c.dir, rel), "utf8");
        const text = MARKED.test(name) ? parseAnnotations(raw).content : raw;
        out.push(`\`${name}\``, "", ...fence(text.trimEnd(), language(name)), "");
    }
    const output = path.join(c.dir, SNAPSHOT);
    if (fs.existsSync(output)) {
        out.push(...fence(fs.readFileSync(output, "utf8").trimEnd(), "toml"), "");
    }
    return out;
}

/// Standalone pages: `features/<topic>.md`, `commands/<topic>.md`,
/// `configuration.md`, by path under the preview directory.
export function previewPages(cases: readonly Case[]): Map<string, string> {
    const pages = new Map<string, string>();
    const shown = documented(cases);
    for (const topic of [...new Set(shown.map((c) => c.topic))].sort()) {
        const command = TOPICS[topic] === "command";
        const lines = [`# ${heading(topic)}`, ""];
        for (const subtopic of [
            ...new Set(shown.filter((c) => c.topic === topic).map((c) => c.subtopic)),
        ]) {
            lines.push(`## ${heading(subtopic)}`, "");
            for (const c of shown.filter((x) => x.topic === topic && x.subtopic === subtopic)) {
                lines.push(...item(c, inline(c, command ? "config" : "all")));
            }
        }
        pages.set(
            `${command ? "commands" : "features"}/${topic}.md`,
            `${lines.join("\n").trimEnd()}\n`,
        );
    }
    const options = shown.filter((c) => c.readme?.option !== undefined);
    if (options.length > 0) {
        const lines = ["# Configuration", ""];
        for (const option of [...new Set(options.map((c) => c.readme?.option ?? ""))].sort()) {
            lines.push(`## \`${option}\``, "");
            for (const c of options.filter((x) => x.readme?.option === option)) {
                lines.push(...item(c, inline(c, "config")));
            }
        }
        pages.set("configuration.md", `${lines.join("\n").trimEnd()}\n`);
    }
    return pages;
}
