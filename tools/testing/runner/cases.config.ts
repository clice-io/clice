import * as os from "node:os";
import { defineConfig } from "vitest/config";

const GB = 1024 ** 3;
/// What the rest of the machine keeps.
const RESERVED = 2 * GB;
/// The most one test holds at once: an answers test runs clice inspect and
/// a server with its workers side by side. Measured as the summed PSS of
/// the clice processes: 1.4 GB in Debug, 0.12 GB in RelWithDebInfo.
const TEST_PEAK = 1.5 * GB;

/// Tests at once: one per core, as many as memory holds.
function concurrency(): number {
    // macOS counts its file cache as used: free memory reads near zero
    // on an idle machine.
    const available = process.platform === "darwin" ? os.totalmem() : os.freemem();
    const fits = Math.floor((available - RESERVED) / TEST_PEAK);
    return Math.max(1, Math.min(os.availableParallelism(), fits));
}

export default defineConfig({
    test: {
        root: import.meta.dirname,
        include: ["cases.ts"],
        globalSetup: ["summary.ts"],
        pool: "forks",
        // Every case is a suite of its own: without this, vitest runs them
        // one after another and only a case's own tests side by side.
        sequence: { concurrent: true },
        maxConcurrency: concurrency(),
        testTimeout: 300_000,
        hookTimeout: 60_000,
    },
});
