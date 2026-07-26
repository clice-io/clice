import vitest from "@vitest/eslint-plugin";
import tseslint from "typescript-eslint";

export default tseslint.config(
    { ignores: ["node_modules/"] },
    ...tseslint.configs.strictTypeChecked,
    ...tseslint.configs.stylisticTypeChecked,
    {
        languageOptions: {
            parserOptions: {
                projectService: true,
                tsconfigRootDir: import.meta.dirname,
            },
        },
        rules: {
            // The harness logs through the runner's console on purpose (server
            // stderr excerpts, snapshot notices).
            "no-console": "off",
            // Positions, ports and exit codes interpolate into messages and
            // snapshot lines everywhere; numbers are fine, the rest stay banned.
            "@typescript-eslint/restrict-template-expressions": ["error", { allowNumber: true }],
        },
    },
    {
        // The config file itself is not part of the TS project; plain-JS lint
        // rules still apply to it.
        files: ["**/*.mjs"],
        extends: [tseslint.configs.disableTypeChecked],
    },
    {
        files: ["**/*.test.ts"],
        plugins: { vitest },
        rules: {
            ...vitest.configs.recommended.rules,
            // vitest's expect(value, message) two-argument form is real API.
            "vitest/valid-expect": ["error", { maxArgs: 2 }],
            // Non-null assertions after an expect() guard are idiomatic in
            // tests; TS cannot narrow through vitest assertions.
            "@typescript-eslint/no-non-null-assertion": "off",
        },
    },
);
