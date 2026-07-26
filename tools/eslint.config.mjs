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
);
