/// Sanitizer flags make the external driver emit paths into its own
/// resource dir (share/*_ignorelist.txt) that clice's resource dir does
/// not carry; a rewritten path aborts cc1 instead of failing a compile.

import type * as proto from "vscode-languageserver-protocol";
import { expect, serve } from "../../fixtures.ts";

const MAIN = "#include <vector>\nint main() { return std::vector<int>{1}.empty(); }\n";

serve.files({ "main.cpp": MAIN }, { manifest: { units: { "main.cpp": ["-fsanitize=address"] } } })(
    "address sanitizer entry compiles",
    async ({ s }) => {
        expect(await s.errors("main.cpp")).toEqual([]);
        const links = (await s.request("textDocument/documentLink", "main.cpp")) as
            proto.DocumentLink[] | null;
        expect(links?.length).toBe(1);
    },
);

serve.files(
    { "sub/ignore.txt": "fun:skipped\n", "sub/main.cpp": MAIN },
    { manifest: { units: {} } },
)("ignorelist beside the entry compiles", async ({ s }) => {
    // The entry's directory is not the root the fixture writes databases for.
    await s.offline(() => {
        s.disk.write(
            "compile_commands.json",
            JSON.stringify([
                {
                    directory: s.workspace.path("sub"),
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
        );
    });
    expect(
        await s.errors("sub/main.cpp"),
        "the list resolves in the entry's directory, as for clang",
    ).toEqual([]);
});
