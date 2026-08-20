import tseslint from "typescript-eslint";

export default tseslint.config(
    { ignores: ["node_modules/", "dist/", "out/", ".vscode-test/"] },
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
            // The extension logs through the output channel and console on
            // purpose (activation notices, server diagnostics).
            "no-console": "off",
            // Positions, ports and exit codes interpolate into messages
            // everywhere; numbers are fine, the rest stay banned.
            "@typescript-eslint/restrict-template-expressions": ["error", { allowNumber: true }],
        },
    },
    {
        // Config files (this one and webpack.config.js) are not part of the
        // TS project; plain-JS lint rules still apply to them.
        files: ["**/*.mjs", "**/*.js"],
        extends: [tseslint.configs.disableTypeChecked],
    },
);
