import tseslint from "typescript-eslint";
import clice from "@clice/tools/lint";

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
        plugins: { clice },
        rules: {
            "clice/timers": "error",
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
