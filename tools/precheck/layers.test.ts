import { describe, expect, test } from "vitest";
import { includeViolations, partitionViolations } from "./layers.ts";

const source = (...lines: string[]): string => lines.join("\n");

describe("includeViolations", () => {
    test("a core layer includes and imports below it, not above", () => {
        expect(
            includeViolations(
                "src/syntax/lexer.cpp",
                "syntax",
                source(
                    '#include "support/logging.h"',
                    "import :vfs.source;",
                    '#include "compile/unit.h"',
                    "export import :semantic.selection;",
                    '  #include "settings/settings.h"',
                ),
            ),
        ).toEqual([
            "src/syntax/lexer.cpp:3: syntax/ must not include compile/unit.h",
            "src/syntax/lexer.cpp:4: syntax/ must not include semantic/selection",
            "src/syntax/lexer.cpp:5: syntax/ must not include settings/settings.h",
        ]);
    });

    test("the top of core reaches no layer outside it", () => {
        expect(
            includeViolations("src/feature/hover.cpp", "feature", "import :worker.pool;"),
        ).toEqual(["src/feature/hover.cpp:1: feature/ must not include worker/pool"]);
    });

    test("analysis reaches project but no worker", () => {
        expect(
            includeViolations(
                "src/analysis/graph.cpp",
                "analysis",
                source("import :project.task_graph;", "import :worker.pool;"),
            ),
        ).toEqual(["src/analysis/graph.cpp:2: analysis/ must not include worker/pool"]);
    });

    test("server reaches project and worker, not analysis or driver", () => {
        expect(
            includeViolations(
                "src/server/master.cpp",
                "server",
                source(
                    "import :project.open;",
                    "import :worker.pool;",
                    "import :analysis.wrap;",
                    '#include "driver/driver.h"',
                ),
            ),
        ).toEqual([
            "src/server/master.cpp:3: server/ must not include analysis/wrap",
            "src/server/master.cpp:4: server/ must not include driver/driver.h",
        ]);
    });

    test("driver may reach every layer", () => {
        expect(
            includeViolations(
                "src/driver/serve.cpp",
                "driver",
                source("import :server.master;", "import :analysis.wrap;"),
            ),
        ).toEqual([]);
    });

    test("system includes, other modules and commented lines are no includes", () => {
        expect(
            includeViolations(
                "src/support/json.cpp",
                "support",
                source(
                    "#include <server/master.h>",
                    "import std;",
                    "import kotatsu;",
                    "// import :server.master;",
                    '// #include "driver/driver.h"',
                ),
            ),
        ).toEqual([]);
    });

    test("the lines are counted across CRLF line ends", () => {
        expect(
            includeViolations(
                "src/vfs/file.cpp",
                "vfs",
                "module;\r\n\r\nimport :command.table;\r\n",
            ),
        ).toEqual(["src/vfs/file.cpp:3: vfs/ must not include command/table"]);
    });
});

describe("partitionViolations", () => {
    test("a partition is named after the layer of its file", () => {
        expect(
            partitionViolations(
                "src/project/graph/task_graph.cppm",
                "project",
                source("module;", "export module clice:project.task_graph;"),
            ),
        ).toEqual([]);
        expect(
            partitionViolations(
                "src/server/master/master.cppm",
                "server",
                source("module;", "", "export module clice:project.master;"),
            ),
        ).toEqual([
            "src/server/master/master.cppm:3: the partition of this file is named server.*",
        ]);
    });

    test("an implementation partition is held to it too", () => {
        expect(
            partitionViolations("src/vfs/table.cppm", "vfs", "module clice:support.table;"),
        ).toEqual(["src/vfs/table.cppm:1: the partition of this file is named vfs.*"]);
    });

    test("the test helpers are named after unittests", () => {
        expect(
            partitionViolations(
                "unittests/utils/tester.cppm",
                "unittests",
                "export module clice:unittests.utils.tester;",
            ),
        ).toEqual([]);
        expect(
            partitionViolations(
                "unittests/utils/tester.cppm",
                "unittests",
                "export module clice:utils.tester;",
            ),
        ).toEqual([
            "unittests/utils/tester.cppm:1: the partition of this file is named unittests.*",
        ]);
    });

    test("the primary interface and other modules declare no partition", () => {
        expect(
            partitionViolations(
                "src/module.cppm",
                "module.cppm",
                source("export module clice;", "export module other:support.x;"),
            ),
        ).toEqual([]);
    });
});
