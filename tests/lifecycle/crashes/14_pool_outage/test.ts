import { expect } from "vitest";
import { generate, test } from "@clice/tools/testing";

// With the only indexing worker past its crash budget, the round parks
// until the pool revives the slot instead of spinning the lost files
// through instant failures, the server answers meanwhile, and the round
// then indexes every file (#611).

const UNITS = 8;

test(
    {
        except: { win32: "kills workers through /proc", darwin: "kills workers through /proc" },
        errors: ["exceeded crash budget"],
    },
    async (t) => {
        t.disk.write(generate.units(UNITS));
        const s = await t.start({
            config: {
                project: {
                    stateless_worker_count: 1,
                    min_stateless_worker_count: 1,
                    max_stateless_worker_count: 1,
                },
            },
            killWorkers: true,
        });
        const parks = await Promise.all(
            Object.keys(generate.units(UNITS)).map((unit) =>
                s.park("tuRun", unit, { at: "worker" }),
            ),
        );
        const waiting = new Set(parks.map((park, index) => ({ park, index })));
        s.open("main.cpp");
        // One more death than the budget of three in a row gives the slot up.
        for (let death = 0; death < 4; death++) {
            const reached = await Promise.race(
                [...waiting].map((entry) => entry.park.reached().then(() => entry)),
            );
            waiting.delete(reached);
            expect(await s.killWorkers("stateless")).toBe(1);
        }
        for (const { park } of waiting) {
            await park.release();
        }
        await t.show("symbols during the outage", s.workspaceSymbols("unit_"));
        // What the round waits on: parked dispatch sends nothing, where a
        // spin would show a round counted far past its files.
        const outage = await s.client.sync({ deadlineMs: 1_000 });
        await t.show("pending during the outage", outage.pending);
        await t.show("sync", { failed: (await s.sync()).failed });
        await t.show("symbols after the revival", s.workspaceSymbols("unit_"));
    },
);
