import { test } from "@clice/tools/testing";

// `${env:NAME}` is the server's environment variable NAME, wherever
// `${workspace}` is substituted.
test("a variable that is set", async (t) => {
    const s = await t.start({ env: { CLICE_CASE_VALUE: "42" } });
    await t.show("main.cpp", s.errors("main.cpp"));
});

// One that is not set is an error at its key, naming it: the project serves
// nothing until it is fixed.
test(
    "a variable that is not set",
    { errors: ["the environment variable CLICE_CASE_VALUE is not set"] },
    async (t) => {
        await t.cli("command main.cpp");
    },
);
