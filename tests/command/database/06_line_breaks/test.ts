import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNCONVERTED_ENTRIES = "of the entries stay driver commands";

// A Windows database's command line splits as the C runtime splits a
// program's: a carriage return and a newline stay inside the argument.
test(
    {
        errors: [MISSING, UNCONVERTED_ENTRIES],
        except: {
            linux: "a database is read as sh splits a command line",
            darwin: "a database is read as sh splits a command line",
        },
    },
    async (t) => {
        await t.cli("command convert compile_commands.json");
    },
);
