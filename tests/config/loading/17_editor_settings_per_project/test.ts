import { test } from "@clice/tools/testing";

// The editor's settings name a configuration only alpha's rules declare:
// beta cannot take them and says so, while alpha serves.
test({ errors: ["initializationOptions: debug names a configuration"] }, async (t) => {
    const s = await t.start({
        folders: ["alpha", "beta"],
        config: { default_configuration: "debug" },
    });
    s.open("alpha/main.cpp");
    s.open("beta/main.cpp");
    await t.show("guidance", await s.guidance());
});
