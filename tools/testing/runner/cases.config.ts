import * as os from "node:os";
import { defineConfig } from "vitest/config";

export default defineConfig({
    test: {
        root: import.meta.dirname,
        include: ["cases.ts"],
        globalSetup: ["summary.ts"],
        pool: "forks",
        maxConcurrency: Math.max(1, os.availableParallelism()),
        testTimeout: 300_000,
        hookTimeout: 60_000,
    },
});
