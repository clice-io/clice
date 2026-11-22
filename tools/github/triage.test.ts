import { describe, expect, test } from "vitest";
import { check, plan, taxonomy, type Proposal, type Snapshot } from "./triage.ts";

const labels = taxonomy(
    [
        '- name: "kind:bug"',
        '- name: "kind:crash"',
        '- name: "feature:hover"',
        '- name: "os:linux"',
        '- name: "os:wsl"',
        '- name: "help wanted"',
        '- name: "needs-triage"',
        '- name: "status:needs-info"',
    ].join("\n"),
);

const snapshot = (
    issues: Record<string, { title: string; state: string; labels: string[] }>,
): Snapshot => ({
    issues: Object.fromEntries(
        Object.entries(issues).map(([number, issue]) => [
            number,
            { ...issue, updatedAt: "2026-11-20T00:00:00Z" },
        ]),
    ),
    digest: { open: 0, untriaged: [], newThisWeek: [], quietWaiting: [] },
});

const proposal = (issue: number, labelSet: string[], extra: Partial<Proposal> = {}): Proposal => ({
    issue,
    labels: labelSet,
    confidence: "high",
    rationale: "why",
    ...extra,
});

describe("check", () => {
    test("a valid proposal adds what the issue lacks and lists what it leaves out", () => {
        const { valid, failures } = check(
            [
                proposal(1, ["kind:bug", "feature:hover", "needs-triage"], {
                    title: "bug(hover): x",
                }),
            ],
            labels,
            snapshot({ "1": { title: "x", state: "OPEN", labels: ["needs-triage", "os:linux"] } }),
        );
        expect(failures).toEqual([]);
        expect(valid[0]?.labels).toEqual(["feature:hover", "kind:bug"]);
        expect(valid[0]?.add).toEqual(["feature:hover", "kind:bug"]);
        expect(valid[0]?.leftOut).toEqual(["os:linux"]);
    });

    test("the rules reject what breaks them", () => {
        const { valid, failures } = check(
            [
                proposal(1, ["kind:bug", "kind:crash"]),
                proposal(2, ["kind:bug", "feature:nope"]),
                proposal(3, ["kind:bug", "help wanted"]),
                proposal(4, ["kind:bug", "os:wsl"]),
                proposal(5, ["kind:bug"], { confidence: "sure" }),
                proposal(9, ["kind:bug"]),
            ],
            labels,
            snapshot({
                "1": { title: "", state: "OPEN", labels: [] },
                "2": { title: "", state: "OPEN", labels: [] },
                "3": { title: "", state: "OPEN", labels: [] },
                "4": { title: "", state: "OPEN", labels: ["os:linux"] },
                "5": { title: "", state: "OPEN", labels: [] },
                "6": { title: "", state: "OPEN", labels: [] },
            }),
        );
        expect(valid).toEqual([]);
        expect(failures).toEqual([
            "#1: 2 kind labels",
            "#2: unknown labels feature:nope",
            "#3: labels for the maintainer alone: help wanted",
            "#4: os:wsl beside a native os label (resolve by hand)",
            '#5: confidence "sure"',
            "#6: no proposal",
            "issues not in the snapshot: #9",
        ]);
    });

    test("a label for the maintainer alone that the issue already has stays", () => {
        const { failures } = check(
            [proposal(1, ["kind:bug", "help wanted"])],
            labels,
            snapshot({ "1": { title: "", state: "OPEN", labels: ["help wanted"] } }),
        );
        expect(failures).toEqual([]);
    });
});

describe("check, the rest of its rules", () => {
    test("several proposals for one issue and an empty title are rejected", () => {
        const { failures } = check(
            [
                proposal(1, ["kind:bug"]),
                proposal(1, ["kind:crash"]),
                proposal(2, ["kind:bug"], { title: " " }),
            ],
            labels,
            snapshot({
                "1": { title: "", state: "OPEN", labels: [] },
                "2": { title: "", state: "OPEN", labels: [] },
            }),
        );
        expect(failures).toEqual(["#1: several proposals", "#2: an empty title"]);
    });

    test("what apply removes itself is not left to the maintainer", () => {
        const { valid } = check(
            [proposal(1, ["kind:crash"])],
            labels,
            snapshot({ "1": { title: "", state: "OPEN", labels: ["needs-triage", "kind:bug"] } }),
        );
        expect(valid[0]?.leftOut).toEqual([]);
    });
});

describe("plan", () => {
    const checked = (labelSet: string[], title?: string) => {
        const { valid } = check(
            [proposal(1, labelSet, title === undefined ? {} : { title })],
            labels,
            snapshot({ "1": { title: "old", state: "OPEN", labels: [] } }),
        );
        const [first] = valid;
        if (first === undefined) {
            throw new Error("the proposal did not pass the check");
        }
        return first;
    };
    const seen = { updatedAt: "2026-11-20T00:00:00Z" };
    const live = (labels: string[], title = "old") => ({
        labels,
        state: "OPEN",
        title,
        updatedAt: seen.updatedAt,
    });
    const options = { keepTitles: false, includeClosed: false };

    test("an issue that changed since the snapshot is skipped", () => {
        const proposalOf = checked(["kind:bug"]);
        const stale = { skip: "it changed since the snapshot (snapshot it again)" };
        // A comment from the reporter, someone's labels, a new title or state:
        // each moves the issue's updatedAt.
        expect(
            plan(proposalOf, { ...live([]), updatedAt: "2026-11-21T00:00:00Z" }, seen, options),
        ).toEqual(stale);
        expect(plan(proposalOf, live([]), undefined, options)).toEqual(stale);
        expect(plan(proposalOf, live(["kind:bug", "kind:crash"]), seen, options)).toEqual({
            skip: "several kinds kind:bug, kind:crash (resolve by hand)",
        });
    });

    test("while needs-triage is on, the proposed kind replaces the template's", () => {
        expect(
            plan(
                checked(["kind:crash", "feature:hover"], "crash(hover): x"),
                live(["needs-triage", "kind:bug"]),
                seen,
                options,
            ),
        ).toEqual({
            add: ["feature:hover", "kind:crash"],
            remove: ["needs-triage", "kind:bug"],
            title: "crash(hover): x",
            notes: [],
        });
    });

    test("the maintainer's kind wins, and takes the title along", () => {
        expect(
            plan(
                checked(["kind:crash", "feature:hover"], "crash(hover): x"),
                live(["kind:bug"]),
                seen,
                options,
            ),
        ).toEqual({
            add: ["feature:hover"],
            remove: [],
            title: undefined,
            notes: [
                "the maintainer's kind:bug wins over kind:crash",
                "title kept: its type came from the dropped kind",
            ],
        });
    });

    test("a closed issue loses its status labels and gains none", () => {
        expect(
            plan(
                checked(["kind:bug"]),
                { ...live(["kind:bug", "status:needs-info"]), state: "CLOSED" },
                seen,
                { keepTitles: false, includeClosed: true },
            ),
        ).toEqual({ add: [], remove: ["status:needs-info"], title: undefined, notes: [] });
    });
});
