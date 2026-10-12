import { expect } from "vitest";
import { generate, test, type Park, type Serve } from "@clice/tools/testing";

// With the only indexing worker past its crash budget, the round parks
// until the pool revives the slot instead of spinning the lost files
// through instant failures, the server answers meanwhile, and the round
// then indexes every file (#611).

const UNITS = 8;

/// Parks for every unit's next index run in its worker.
function parkAll(s: Serve): Promise<Park[]> {
    return Promise.all(
        Object.keys(generate.units(UNITS)).map((unit) => s.park("tuRun", unit, { at: "worker" })),
    );
}

/// The first of `parks` its work reaches.
function firstReached(parks: readonly Park[]): Promise<Park> {
    return Promise.race(parks.map((park) => park.reached().then(() => park)));
}

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
        let parks = await parkAll(s);
        s.open("main.cpp");
        // One more death than the budget of three in a row gives the slot up.
        for (let death = 0; death < 4; death++) {
            const reached = await firstReached(parks);
            parks = parks.filter((park) => park !== reached);
            expect(await s.killWorkers("stateless")).toBe(1);
        }
        for (const park of parks) {
            await park.release();
        }
        await t.show("symbols during the outage", s.workspaceSymbols("unit_"));
        // The first run the revived slot starts finds the round as the
        // outage left it: parked dispatch sent nothing, where a spin would
        // show a round counted far past its files. The raw client: what
        // sync reports still running is the observation.
        const revival = await parkAll(s);
        await firstReached(revival);
        await t.show("pending at the revival", (await s.client.sync({ deadlineMs: 0 })).pending);
        for (const park of revival) {
            await park.release();
        }
        await t.show("sync", { failed: (await s.sync()).failed });
        await t.show("symbols after the revival", s.workspaceSymbols("unit_"));
    },
);
