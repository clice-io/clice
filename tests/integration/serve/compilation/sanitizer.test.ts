/// Sanitizer flags make the external driver emit paths into its own
/// resource dir (share/*_ignorelist.txt) that clice's resource dir does
/// not carry; a rewritten path aborts cc1 instead of failing a compile.

import type * as proto from "vscode-languageserver-protocol";
import { expect, serve } from "../../fixtures.ts";

const MAIN = "#include <vector>\nint main() { return std::vector<int>{1}.empty(); }\n";

serve("stdlib", { units: { "report.cpp": ["-fsanitize=address"] } })(
    "address sanitizer entry compiles",
    async ({ s }) => {
        expect(await s.errors("report.cpp")).toEqual([]);
        const links = await s.request<proto.DocumentLink[] | null>(
            "textDocument/documentLink",
            "report.cpp",
        );
        expect(links?.length, "each standard header is linked").toBe(3);
    },
);

serve.files(
    {
        "sub/ignore.txt": "fun:skipped\n",
        "sub/main.cpp": MAIN,
        "compile_commands.json": (workspace) =>
            JSON.stringify([
                {
                    directory: workspace.path("sub"),
                    file: "main.cpp",
                    arguments: [
                        "clang++",
                        "-fsanitize=address",
                        "-fsanitize-ignorelist=ignore.txt",
                        "-c",
                        "main.cpp",
                    ],
                },
            ]),
    },
    { databases: false },
)("ignorelist beside the entry compiles", async ({ s }) => {
    expect(
        await s.errors("sub/main.cpp"),
        "the list resolves in the entry's directory, as for clang",
    ).toEqual([]);
});
