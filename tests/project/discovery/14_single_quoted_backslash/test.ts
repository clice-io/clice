import { test } from "@clice/tools/testing";

// The command string reads as sh reads it: a backslash inside single quotes
// is a literal character, so S is "a\\b".
test(
    {
        except: {
            win32: "a command string reads with Windows rules there, where single quotes do not quote",
        },
    },
    async (t) => {
        const s = await t.start();
        await t.show("main.cpp", s.errors("main.cpp"));
    },
);
