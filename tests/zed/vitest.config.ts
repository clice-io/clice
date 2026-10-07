import { defineConfig } from "vitest/config";

/// Each test downloads or starts clice through a real Zed, building on the
/// installation the first one made.
export default defineConfig({
    test: {
        include: ["zed/**/*.test.ts"],
        testTimeout: 600_000,
    },
});
