/// Sanitizer flags make the external driver emit paths into its own
/// resource dir (share/*_ignorelist.txt) that clice's resource dir does
/// not carry; a rewritten path aborts cc1 instead of failing a compile.

import { expect, test } from "../fixtures.ts";

test("address sanitizer entry compiles", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write(
        "main.cpp",
        "#include <vector>\nint main() { return std::vector<int>{1}.empty(); }\n",
    );
    workspace.writeCDB(["main.cpp"], { extraArgs: ["-fsanitize=address"] });
    await client.initialize(workspace);

    const [uri] = await client.openAndWait("main.cpp");
    client.assertNoErrors(uri);
    expect((await client.documentLinks(uri))?.length).toBe(1);
});

test("ignorelist beside the entry compiles", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("ignore.txt", "fun:skipped\n");
    workspace.write("main.cpp", "int main() { return 0; }\n");
    workspace.writeCDB(["main.cpp"], {
        extraArgs: ["-fsanitize=address", "-fsanitize-ignorelist=ignore.txt"],
    });
    await client.initialize(workspace);

    const [uri] = await client.openAndWait("main.cpp");
    client.assertNoErrors(uri, "the list resolves in the entry's directory, as for clang");
});
