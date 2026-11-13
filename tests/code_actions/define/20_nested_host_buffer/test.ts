import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The header's host is a file the nested project serves, open with unsaved
// edits: the definition goes into its buffer, after what the buffer
// defines, and names the buffer's version.
test(async (t) => {
    const s = await t.start({ folders: ["repo"] });
    await s.indexed();
    s.open("repo/sub/widget.cpp");
    s.edit("repo/sub/widget.cpp", {
        after: "int helper() { return 0; }\n",
        insert: "\nint later() { return 1; }\n",
    });
    await s.diagnostics("repo/sub/widget.cpp");
    s.open("repo/widget.h");
    await s.diagnostics("repo/widget.h");
    const { uri, position } = s.position(t.marker("size"));
    const actions = await s.client.codeActions(uri, { start: position, end: position });
    const versions = (actions ?? []).flatMap((action) =>
        "edit" in action
            ? (action.edit.documentChanges ?? []).flatMap((change) =>
                  "textDocument" in change &&
                  s.spelling(change.textDocument.uri) === "repo/sub/widget.cpp"
                      ? [change.textDocument.version]
                      : [],
              )
            : [],
    );
    expect(versions).toEqual([1]);
    await t.show("actions", s.codeActions(t.marker("size")));
});
