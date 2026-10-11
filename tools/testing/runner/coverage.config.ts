import { defineConfig } from "vitest/config";

export default defineConfig({
    test: {
        root: import.meta.dirname,
        include: ["coverage.ts"],
        testTimeout: 120_000,
    },
});
