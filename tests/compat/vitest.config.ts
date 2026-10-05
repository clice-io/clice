import { defineConfig } from "vitest/config";

/// Each scenario builds a project with real tools and runs clice over the
/// result; scenarios run one after another, the builds and clice's worker
/// pool already use the machine's cores.
export default defineConfig({
    test: {
        include: ["compat/**/*.test.ts"],
        testTimeout: 300_000,
    },
});
