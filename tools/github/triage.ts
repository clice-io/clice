/// Triage of the untriaged issues (the triage-issues skill): a snapshot to classify
/// from, the check of the proposed labels and titles against the taxonomy
/// and the rules, and their application once the maintainer approved.
///
/// Usage:
///   node tools/github/triage.ts snapshot [--state all] [--limit <n>]
///   node tools/github/triage.ts check <labels.json>
///   node tools/github/triage.ts apply <labels.json> [--only N,N | --skip N,N] [--keep-titles] [--include-closed]
///
/// `snapshot` writes each untriaged issue to temp/triage/issues/<N>.md and
/// what it saw to temp/triage/snapshot.json; `check` and `apply` read it.

import * as fs from "node:fs";
import * as path from "node:path";
import { parseArgs } from "node:util";
import { REPO_ROOT } from "../repo.ts";
import { gh, ghJson } from "./gh.ts";

const OUT = path.join(REPO_ROOT, "temp", "triage");
const SNAPSHOT = path.join(OUT, "snapshot.json");

const BODY_LIMIT = 8000;
const COMMENT_LIMIT = 3000;
/// The comments an issue file keeps: the first and the last half of these.
const COMMENT_WINDOW = 30;
const MAINTAINERS = new Set(["16bit-ykiko"]);

const FORBIDDEN = new Set(["good first issue", "help wanted"]);
const MARKERS = new Set(["needs-triage", "triaged"]);
const NATIVE_OS = new Set(["os:linux", "os:macos", "os:windows"]);
const CONFIDENCE = new Set(["high", "medium", "low"]);

/// One issue's proposal, as rules.md specifies the file.
export interface Proposal {
    issue: number;
    labels: string[];
    confidence: string;
    rationale: string;
    title?: string | null;
    ask?: string | null;
    gap?: string | null;
}

export interface Snapshot {
    issues: Record<string, { title: string; state: string; labels: string[] }>;
    digest: {
        open: number;
        untriaged: number[];
        newThisWeek: [number, string][];
        quietWaiting: [number, string, number][];
    };
}

/// A proposal that passed the check, with the labels it adds and those the
/// issue has that it leaves out (removed by hand only).
export interface Checked extends Proposal {
    add: string[];
    leftOut: string[];
}

/// Every label name of .github/labels.yml.
export function taxonomy(labelsYml: string): Set<string> {
    return new Set([...labelsYml.matchAll(/^- name: "(.+)"$/gm)].map((match) => match[1] ?? ""));
}

/// The proposals held to the taxonomy and the rules, and to the snapshot:
/// every snapshotted issue has exactly one proposal, and no other issue has.
export function check(
    proposals: readonly Proposal[],
    labels: ReadonlySet<string>,
    snapshot: Snapshot,
): { valid: Checked[]; failures: string[] } {
    const failures: string[] = [];
    const byIssue = new Map<number, Proposal[]>();
    for (const proposal of proposals) {
        byIssue.set(proposal.issue, [...(byIssue.get(proposal.issue) ?? []), proposal]);
    }
    const valid: Checked[] = [];
    for (const number of Object.keys(snapshot.issues)
        .map(Number)
        .sort((a, b) => a - b)) {
        const found = byIssue.get(number) ?? [];
        if (found.length !== 1) {
            failures.push(
                `#${number}: ${found.length === 0 ? "no proposal" : "several proposals"}`,
            );
            continue;
        }
        const [proposal] = found;
        if (proposal === undefined) {
            continue;
        }
        const existing = new Set(snapshot.issues[String(number)]?.labels ?? []);
        const proposed = new Set(proposal.labels.filter((label) => !MARKERS.has(label)));
        const problems: string[] = [];
        const unknown = [...proposed].filter((label) => !labels.has(label));
        if (unknown.length > 0) {
            problems.push(`unknown labels ${unknown.join(", ")}`);
        }
        const kinds = [...proposed].filter((label) => label.startsWith("kind:"));
        if (kinds.length !== 1) {
            problems.push(`${kinds.length} kind labels`);
        }
        const forbidden = [...proposed].filter(
            (label) => FORBIDDEN.has(label) && !existing.has(label),
        );
        if (forbidden.length > 0) {
            problems.push(`labels for the maintainer alone: ${forbidden.join(", ")}`);
        }
        const all = new Set([...proposed, ...existing]);
        if (all.has("os:wsl") && [...all].some((label) => NATIVE_OS.has(label))) {
            problems.push("os:wsl beside a native os label (resolve by hand)");
        }
        if (!CONFIDENCE.has(proposal.confidence)) {
            problems.push(`confidence ${JSON.stringify(proposal.confidence)}`);
        }
        if (proposal.title?.trim() === "") {
            problems.push("an empty title");
        }
        if (problems.length > 0) {
            failures.push(`#${number}: ${problems.join("; ")}`);
            continue;
        }
        valid.push({
            ...proposal,
            labels: [...proposed].sort(),
            add: [...proposed].filter((label) => !existing.has(label)).sort(),
            leftOut: [...existing]
                .filter((label) => !proposed.has(label) && !MARKERS.has(label))
                .sort(),
        });
    }
    const extra = [...byIssue.keys()].filter((number) => !(String(number) in snapshot.issues));
    if (extra.length > 0) {
        failures.push(
            `issues not in the snapshot: ${extra.map((number) => `#${number}`).join(", ")}`,
        );
    }
    return { valid, failures };
}

function author(entry: { login: string } | null | undefined): string {
    const name = entry?.login ?? "ghost";
    return MAINTAINERS.has(name) ? `${name} (maintainer)` : name;
}

function daysSince(iso: string): number {
    return (Date.now() - Date.parse(iso)) / 86_400_000;
}

async function snapshot(state: string, limit: number): Promise<void> {
    fs.rmSync(OUT, { recursive: true, force: true });
    fs.mkdirSync(path.join(OUT, "issues"), { recursive: true });
    const listed = await ghJson<
        {
            number: number;
            title: string;
            state: string;
            labels: { name: string }[];
            createdAt: string;
            updatedAt: string;
        }[]
    >([
        "issue",
        "list",
        "--state",
        state,
        "--limit",
        "1000",
        "--json",
        "number,title,labels,state,createdAt,updatedAt",
    ]);
    if (listed.length === 1000) {
        console.log("warning: the listing stopped at 1000 issues; the snapshot may miss some");
    }
    const names = (issue: { labels: { name: string }[] }): string[] =>
        issue.labels.map((label) => label.name).sort();
    const open = listed.filter((issue) => issue.state === "OPEN");
    const untriaged = listed.filter((issue) => !names(issue).includes("triaged"));
    const selected = limit > 0 ? untriaged.slice(0, limit) : untriaged;
    const waiting = new Set(["status:needs-info", "status:needs-repro"]);
    const result: Snapshot = {
        issues: {},
        digest: {
            open: open.length,
            untriaged: untriaged.map((issue) => issue.number).sort((a, b) => a - b),
            newThisWeek: open
                .filter((issue) => daysSince(issue.createdAt) <= 7)
                .map((issue) => [issue.number, issue.title]),
            quietWaiting: open
                .filter(
                    (issue) =>
                        names(issue).some((label) => waiting.has(label)) &&
                        daysSince(issue.updatedAt) > 14,
                )
                .map((issue) => [
                    issue.number,
                    issue.title,
                    Math.floor(daysSince(issue.updatedAt)),
                ]),
        },
    };
    for (const issue of selected) {
        const detail = await ghJson<{
            number: number;
            title: string;
            body: string;
            author: { login: string } | null;
            comments: { author: { login: string } | null; body: string }[];
        }>(["issue", "view", String(issue.number), "--json", "number,title,body,author,comments"]);
        const body = detail.body || "(no body)";
        const lines = [
            `# Issue #${detail.number}: ${detail.title}`,
            `State: ${issue.state}  Author: ${author(detail.author)}`,
            `Existing labels: ${names(issue).join(", ") || "(none)"}`,
            "",
            body.length > BODY_LIMIT
                ? `${body.slice(0, BODY_LIMIT)}\n[... body truncated ...]`
                : body,
        ];
        const half = COMMENT_WINDOW / 2;
        const omitted = detail.comments.length - COMMENT_WINDOW;
        detail.comments.forEach((comment, index) => {
            if (omitted > 0 && index === half) {
                lines.push("", `[... ${omitted} comments omitted ...]`);
            }
            if (omitted > 0 && index >= half && index < half + omitted) {
                return;
            }
            lines.push(
                "",
                `--- comment by ${author(comment.author)} ---`,
                comment.body.slice(0, COMMENT_LIMIT),
            );
        });
        fs.writeFileSync(path.join(OUT, "issues", `${detail.number}.md`), `${lines.join("\n")}\n`);
        result.issues[String(detail.number)] = {
            title: detail.title,
            state: issue.state,
            labels: names(issue),
        };
    }
    fs.writeFileSync(SNAPSHOT, `${JSON.stringify(result, null, 1)}\n`);
    const { digest } = result;
    console.log(
        `untriaged: ${selected.length}${selected.length === untriaged.length ? "" : ` of ${untriaged.length}`} in ${path.relative(process.cwd(), path.join(OUT, "issues"))}/`,
    );
    console.log(`open: ${digest.open}`);
    console.log(
        `new this week (${digest.newThisWeek.length}): ${digest.newThisWeek.map(([number]) => `#${number}`).join(", ") || "none"}`,
    );
    console.log(
        `waiting on the reporter, quiet for two weeks: ${digest.quietWaiting.map(([number, , days]) => `#${number} (${days} days)`).join(", ") || "none"}`,
    );
}

function load(file: string): { valid: Checked[]; failures: string[] } {
    const labels = taxonomy(fs.readFileSync(path.join(REPO_ROOT, ".github", "labels.yml"), "utf8"));
    const snap = JSON.parse(fs.readFileSync(SNAPSHOT, "utf8")) as Snapshot;
    return check(JSON.parse(fs.readFileSync(file, "utf8")) as Proposal[], labels, snap);
}

function printCheck(file: string): void {
    const { valid, failures } = load(file);
    for (const proposal of valid) {
        console.log(
            `#${proposal.issue} [${proposal.confidence}] +${proposal.add.join(", ") || "nothing"} — ${proposal.rationale}`,
        );
        const notes: [string, string | null | undefined][] = [
            ["title →", proposal.title],
            ["remove by hand →", proposal.leftOut.join(", ")],
            ["ask →", proposal.ask],
            ["no label for →", proposal.gap],
        ];
        for (const [what, value] of notes) {
            if (value) {
                console.log(`    ${what} ${value}`);
            }
        }
    }
    console.log(`\nvalid: ${valid.length} · rejected: ${failures.length}`);
    for (const failure of failures) {
        console.log(`REJECTED ${failure}`);
    }
}

function numbers(text: string | undefined): Set<number> {
    return new Set(text === undefined || text === "" ? [] : text.split(",").map(Number));
}

async function apply(
    file: string,
    options: { only: Set<number>; skip: Set<number>; keepTitles: boolean; includeClosed: boolean },
): Promise<void> {
    const { valid, failures } = load(file);
    for (const failure of failures) {
        console.log(`not applied ${failure}`);
    }
    const snap = JSON.parse(fs.readFileSync(SNAPSHOT, "utf8")) as Snapshot;
    for (const proposal of valid) {
        const number = proposal.issue;
        if ((options.only.size > 0 && !options.only.has(number)) || options.skip.has(number)) {
            continue;
        }
        const seen = snap.issues[String(number)];
        const live = await ghJson<{ labels: { name: string }[]; state: string; title: string }>([
            "issue",
            "view",
            String(number),
            "--json",
            "labels,state,title",
        ]);
        const liveLabels = new Set(live.labels.map((label) => label.name));
        const closed = live.state !== "OPEN";
        // The proposal was made for the snapshot's issue: a changed state, or
        // someone else's triage meanwhile, makes it stale.
        if (live.state !== seen?.state) {
            console.log(`#${number} skipped: its state changed since the snapshot`);
            continue;
        }
        if (liveLabels.has("triaged")) {
            console.log(`#${number} skipped: triaged since the snapshot`);
            continue;
        }
        if (closed && !options.includeClosed) {
            console.log(`#${number} skipped: closed`);
            continue;
        }
        const marker = liveLabels.has("needs-triage");
        const liveKinds = [...liveLabels].filter((label) => label.startsWith("kind:")).sort();
        if (!marker && liveKinds.length > 1) {
            console.log(
                `#${number} skipped: several kinds ${liveKinds.join(", ")} (resolve by hand)`,
            );
            continue;
        }
        const kind = proposal.labels.find((label) => label.startsWith("kind:"));
        let add = proposal.labels.filter((label) => !liveLabels.has(label));
        const remove = marker ? ["needs-triage"] : [];
        if (closed) {
            add = add.filter((label) => !label.startsWith("status:"));
            remove.push(...[...liveLabels].filter((label) => label.startsWith("status:")).sort());
        }
        // The maintainer's kind wins over the proposed one; the status labels
        // and the title depend on the kind, so they go with it.
        const maintainerKind =
            !marker && liveKinds.length > 0 && kind !== undefined && !liveKinds.includes(kind);
        if (marker) {
            remove.push(...liveKinds.filter((label) => label !== kind));
        } else if (maintainerKind) {
            add = add.filter((label) => !label.startsWith("kind:") && !label.startsWith("status:"));
            console.log(`#${number} the maintainer's ${liveKinds.join(", ")} wins over ${kind}`);
        }
        const all = new Set([...liveLabels, ...add]);
        if (all.has("os:wsl") && [...all].some((label) => NATIVE_OS.has(label))) {
            console.log(`#${number} skipped: os:wsl beside a native os label (resolve by hand)`);
            continue;
        }
        if (!options.keepTitles && proposal.title && proposal.title !== live.title) {
            if (maintainerKind) {
                console.log(`#${number} title kept: its type came from the dropped kind`);
            } else if (live.title !== seen.title) {
                console.log(`#${number} title kept: it changed since the snapshot`);
            } else {
                await gh(["issue", "edit", String(number), "--title", proposal.title]);
                console.log(`#${number} title → ${proposal.title}`);
            }
        }
        if (remove.length > 0) {
            await gh(["issue", "edit", String(number), "--remove-label", remove.join(",")]);
        }
        if (add.length > 0) {
            await gh(["issue", "edit", String(number), "--add-label", add.join(",")]);
        }
        // `triaged` hides the issue from every later snapshot: it goes on
        // last, so a failed edit before it leaves the issue to the next one.
        await gh(["issue", "edit", String(number), "--add-label", "triaged"]);
        console.log(
            `#${number} +${[...add, "triaged"].join(", ")}${remove.length > 0 ? ` -${remove.join(", ")}` : ""}`,
        );
    }
}

async function main(): Promise<void> {
    const { values, positionals } = parseArgs({
        options: {
            state: { type: "string", default: "open" },
            limit: { type: "string", default: "0" },
            only: { type: "string" },
            skip: { type: "string" },
            "keep-titles": { type: "boolean", default: false },
            "include-closed": { type: "boolean", default: false },
        },
        allowPositionals: true,
    });
    const [command, file] = positionals;
    if (command === "snapshot") {
        await snapshot(values.state === "all" ? "all" : "open", Number(values.limit));
    } else if (command === "check" && file !== undefined) {
        printCheck(file);
    } else if (command === "apply" && file !== undefined) {
        await apply(file, {
            only: numbers(values.only),
            skip: numbers(values.skip),
            keepTitles: values["keep-titles"],
            includeClosed: values["include-closed"],
        });
    } else {
        console.error(
            "usage: node tools/github/triage.ts snapshot [--state all] | check <labels.json> | apply <labels.json> [--only N,N | --skip N,N] [--keep-titles] [--include-closed]",
        );
        process.exit(2);
    }
}

if (import.meta.main) {
    await main();
}
