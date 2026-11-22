import { describe, expect, test } from "vitest";
import { check, taxonomy, type Proposal, type Snapshot } from "./triage.ts";

const labels = taxonomy(
    [
        '- name: "kind:bug"',
        '- name: "kind:crash"',
        '- name: "feature:hover"',
        '- name: "os:linux"',
        '- name: "os:wsl"',
        '- name: "help wanted"',
        '- name: "needs-triage"',
    ].join("\n"),
);

const snapshot = (issues: Snapshot["issues"]): Snapshot => ({
    issues,
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
