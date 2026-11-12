import { expect, test } from "vitest";
import { planned, type Plan } from "./modularize.ts";
import type { Entry } from "./project.ts";

const WS = "/ws";

function entry(file: string, args: string[] = []): Entry {
    return {
        directory: WS,
        file: `${WS}/${file}`,
        arguments: ["clang++", "-std=c++20", ...args, "-fsyntax-only", `${WS}/${file}`],
    };
}

function plan(parts: {
    wrapping?: Partial<Plan["wrapping"]>;
    rewriting?: Partial<Plan["rewriting"]>;
}): Plan {
    return {
        wrapping: {
            stdSources: [],
            modules: [],
            mirrors: [],
            prelude: "prelude.h",
            ...parts.wrapping,
        },
        rewriting: { modules: [], importers: [], ...parts.rewriting },
    };
}

const wrapped = plan({
    wrapping: {
        stdSources: ["stdmod/std.cppm"],
        modules: [
            {
                name: "alpha",
                source: "alpha.cppm",
                includeRoots: ["/ws/third/alpha"],
                mirrors: ["mirror/std"],
            },
        ],
        mirrors: ["mirror/std", "mirror/alpha"],
    },
    rewriting: {
        modules: [
            {
                primary: "app/core/module.cppm",
                interfaces: ["app/core/text.cppm"],
                partitions: [],
                sources: ["app/core/text.cpp"],
            },
        ],
        importers: ["app/main.cpp"],
    },
});

const read = [
    entry("app/core/text.cpp"),
    entry("app/main.cpp", ["-DMAIN"]),
    entry("app/other.cpp"),
    entry("app/gone.cpp"),
];

test("the units of the build a plan describes", () => {
    const database = planned(read, wrapped, "wrap", (file) => file !== "app/gone.cpp");
    expect(database.map((unit) => unit.arguments.join(" "))).toEqual([
        "clang++ -std=c++20 -fsyntax-only /ws/stdmod/std.cppm",
        "clang++ -Iwrap/mirror/std -I/ws/third/alpha -std=c++20 -fsyntax-only /ws/wrap/alpha.cppm",
        "clang++ -I. -std=c++20 -fsyntax-only /ws/app/core/module.cppm",
        "clang++ -I. -std=c++20 -fsyntax-only /ws/app/core/text.cppm",
        "clang++ -I. -std=c++20 -fsyntax-only /ws/app/core/text.cpp",
        "clang++ -Iwrap/mirror/std -Iwrap/mirror/alpha -I. -std=c++20 -DMAIN -fsyntax-only /ws/app/main.cpp",
        "clang++ -Iwrap/mirror/std -Iwrap/mirror/alpha -I. -include wrap/prelude.h -std=c++20 -fsyntax-only /ws/app/other.cpp",
    ]);
    expect(database.every((unit) => unit.directory === WS)).toBe(true);
});

test("a merged build takes its configuration's sources and roots", () => {
    const merged = plan({
        wrapping: {
            modules: [
                { name: "alpha", source: "alpha.cppm", includeRoots: [], mirrors: ["mirror/std"] },
            ],
            mirrors: ["mirror/std"],
        },
    });
    const database = planned([entry("app/main.cpp")], merged, "merged", () => true, {
        args: ["-DONE"],
        own: wrapped,
    });
    expect(database.map((unit) => unit.arguments.join(" "))).toEqual([
        "clang++ -DONE -std=c++20 -fsyntax-only /ws/stdmod/std.cppm",
        "clang++ -DONE -Imerged/mirror/std -I/ws/third/alpha -std=c++20 -fsyntax-only /ws/merged/alpha.cppm",
        "clang++ -DONE -Imerged/mirror/std -I. -include merged/prelude.h -std=c++20 -fsyntax-only /ws/app/main.cpp",
    ]);
    expect(() =>
        planned([entry("app/main.cpp")], wrapped, "merged", () => true, { own: plan({}) }),
    ).toThrow("the configuration's plan wraps no module alpha");
    expect(() => planned([], wrapped, "wrap", () => true)).toThrow(
        "the run read no database entry",
    );
});
