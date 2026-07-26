import { defineConfig } from "vitest/config";

export default defineConfig({
  test: {
    include: ["integration/**/*.test.ts"],
    pool: "forks",
    // Tests within one file run sequentially; files run in parallel.
    // Same-workspace exclusivity across files is enforced by the session
    // fixture's per-workspace lock, not by the scheduler.
    fileParallelism: true,
    testTimeout: 60_000,
    hookTimeout: 60_000,
    globalSetup: ["./tools/global_setup.ts"],
  },
});
