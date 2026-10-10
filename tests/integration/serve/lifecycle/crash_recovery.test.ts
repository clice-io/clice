/// Crash-recovery of background indexing.

import type { Hold } from "@clice/tools/actions";
import { expect, serve } from "../../fixtures.ts";

// An index run whose worker is killed from outside — the death names no
// request — is lost, not blamed: a later round indexes the file again.
serve("shapes/headers", { killOn: { request: "tuRun", file: "src/registry.cpp" } })(
    "crash during indexing",
    async ({ s }) => {
        await s.indexed();
        const counts = await s.counts();
        for (const unit of Object.keys(s.manifest.units ?? {})) {
            expect(counts.files[unit]?.index, unit).toBe(unit === "src/registry.cpp" ? 2 : 1);
        }
        expect(s.show(await s.workspaceSymbols("registry_count"))).toBe(
            "registry_count src/registry.cpp: int registry_count() {",
        );
        expect(s.workspace.log("master.log")).toContain("Worker died while indexing");
    },
);

const FILE_COUNT = 8;
/// The pool's max_crash_streak: a slot is given up at the death past it.
const CRASH_BUDGET = 3;

// Light units: a worker's crashes add up only while it dies within the
// pool's healthy uptime of its spawn, and the holds keep the round in
// flight however fast the runs are.
const OUTAGE_FILES: Record<string, string> = { "main.cpp": "int main() { return 0; }\n" };
for (let i = 0; i < FILE_COUNT; i++) {
    OUTAGE_FILES[`file_${i}.cpp`] = `int func_${i}() { return ${i}; }\n`;
}

// Regression for #611: with every stateless slot's crash budget burnt,
// the dispatch loop must park until the pool revives a slot — not spin
// the same requeued files through instant worker-unavailable failures
// (the incident produced a ~986 GiB master.log with a [51294/1]
// progress numerator).
serve
    .files(OUTAGE_FILES, {
        // One stateless slot only, so exhausting its crash budget darkens
        // the whole pool.
        config: {
            project: {
                stateless_worker_count: 1,
                min_stateless_worker_count: 1,
                max_stateless_worker_count: 1,
                idle_timeout_ms: 10,
            },
        },
        anomalies: true,
    })
    .skipIf(process.platform !== "linux")("pool outage parks indexing", async ({ s }) => {
    const indexed = async () =>
        new Set(((await s.workspaceSymbols("func_")) ?? []).map((symbol) => symbol.name));

    // Every index reply parks until the case lets it go: one arriving
    // tells that a stateless worker is up, the one to kill.
    const holds = new Map<Hold, string>();
    const watch = async (file: string) => {
        holds.set(await s.hold("index", file), file);
    };
    for (const unit of Object.keys(s.manifest.units ?? {})) {
        await watch(unit);
    }
    await s.compiled("main.cpp");

    // Kill each worker on sight until the slot's crash budget is spent.
    // SIGUSR2 names no request and, unlike a kill from outside
    // (SIGKILL) of an idle worker, counts as the worker failing by
    // itself.
    const killed = new Set<number>();
    while (killed.size <= CRASH_BUDGET) {
        const parked = await s.firstParked([...holds.keys()]);
        const file = holds.get(parked) ?? "";
        holds.delete(parked);
        for (const pid of s.client.workerPids("SL-")) {
            if (!killed.has(pid)) {
                process.kill(pid, "SIGUSR2");
                killed.add(pid);
            }
        }
        await parked.release();
        // Its run may come again: lost, or requeued.
        await watch(file);
    }
    expect(killed.size, "no stateless worker was ever seen").toBeGreaterThanOrEqual(3);
    for (const hold of holds.keys()) {
        await hold.release();
    }

    // Dark window: the master answers while the round is parked on the
    // capacity signal.
    const during = await indexed();
    const expected = Array.from({ length: FILE_COUNT }, (_, i) => `func_${i}`);
    expect(
        expected.filter((name) => during.has(name)).length,
        "the outage must strike mid-round",
    ).toBeLessThan(FILE_COUNT);

    // The revival cooldown (30s) re-arms the slot and the parked round
    // must resume and finish every file: lost runs requeue past the
    // round snapshot, so no single file burns its budget.
    await s.indexed();
    const found = await indexed();
    const missing = expected.filter((name) => !found.has(name));
    expect(missing, `missing after outage (had ${during.size} during)`).toEqual([]);
    const log = s.workspace.log("master.log");
    expect(log.includes("exceeded crash budget"), "the pool never went dark").toBe(true);
    expect(log).toContain("[anomaly:WorkerCrash]");

    // The spin itself: parked dispatch sends nothing, so the outage may
    // produce at most a handful of worker-unavailable requeues — the
    // incident produced them at an unbounded rate.
    const requeues = log.match(/No stateless workers available/g);
    expect((requeues ?? []).length).toBeLessThanOrEqual(FILE_COUNT);
});

// A file whose own index run crashes its worker is not requeued: the
// same bytes would crash the next run too. It is retried once it
// changes.
serve
    .files(
        {
            "poison.cpp": "int poison_fn() { return 1; }\n",
            "healthy.cpp": "int healthy_fn() { return 2; }\n",
            "main.cpp": "int main() { return 0; }\n",
        },
        { crashOn: { request: "tuRun", file: "poison.cpp" } },
    )
    .skipIf(process.platform !== "linux")("index crash waits for change", async ({ s }) => {
    await s.compiled("main.cpp");
    const crashes = () =>
        s.workspace.workerCrashes(`tuRun ${s.workspace.displayPath("poison.cpp")}`);
    const symbols = async () =>
        new Set(((await s.workspaceSymbols("_fn")) ?? []).map((symbol) => symbol.name));

    const { failed } = await s.sync();
    expect(failed).toEqual(["poison.cpp"]);
    expect((await symbols()).has("healthy_fn")).toBe(true);
    expect(crashes()).toBe(1);
    const log = s.workspace.log("master.log");
    expect(log).toContain("Index giving up on");
    expect(log).toContain("[anomaly:WorkerCrash]");

    // A change is the retry.
    s.disk.write("poison.cpp", "int poison_fn() { return 300; }\n");
    await s.sync({ poll: true });
    expect(crashes()).toBe(2);
    expect((await symbols()).has("poison_fn")).toBe(false);
});
