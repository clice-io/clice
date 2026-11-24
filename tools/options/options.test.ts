import { describe, expect, test } from "vitest";
import {
    bucket,
    cc1Arguments,
    cc1Table,
    change,
    compileJob,
    counterpartBase,
    difference,
    type Evidence,
    families,
    finding,
    type Option,
    optionClasses,
    parseClassTable,
    parseOptions,
    suspicious,
} from "./options.ts";

const HELP_TEXTS =
    "(std::array<std::pair<std::array<unsigned, 2>, const char*>, 1>{{ {std::array<unsigned, 2>{{0, 0}}, nullptr} }})";

/// An entry as Options.inc writes it, its help text on a line of its own.
function entry(
    macro: string,
    spelling: string,
    [id, kind, group, alias, flags, visibility]: readonly string[],
    values = "nullptr",
    marshalling = "",
): string {
    return (
        `${macro}(1, 7 /* ${spelling} */, ${id ?? ""}, ${kind ?? ""}, ${group ?? ""}, ${alias ?? ""}, ` +
        `nullptr, ${flags ?? ""}, ${visibility ?? ""}, 0,\n` +
        `       "Help (with commas, parentheses) and \\"quotes\\"", ${HELP_TEXTS}, nullptr, ${values}, 0` +
        `${marshalling})`
    );
}

const OPTIONS_INC = [
    "#ifdef OPTION",
    entry("OPTION", "<CompileOnly group>", [
        "CompileOnly_Group",
        "Group",
        "INVALID",
        "INVALID",
        "0",
        "0",
    ]),
    entry("OPTION", "<f group>", ["f_Group", "Group", "CompileOnly_Group", "INVALID", "0", "0"]),
    entry("OPTION", "<ignored f group>", [
        "clang_ignored_f_Group",
        "Group",
        "f_Group",
        "INVALID",
        "0",
        "0",
    ]),
    entry("OPTION", "<M group>", ["M_Group", "Group", "INVALID", "INVALID", "0", "0"]),
    entry("OPTION", "-fplt", ["fplt", "Flag", "f_Group", "INVALID", "0", "DefaultVis"]),
    entry("OPTION", "-fno-plt", [
        "fno_plt",
        "Flag",
        "f_Group",
        "INVALID",
        "0",
        "DefaultVis | CC1Option | DefaultVis",
    ]),
    entry("OPTION", "-fident", [
        "fident",
        "Flag",
        "clang_ignored_f_Group",
        "INVALID",
        "NoXarchOption | Ignored",
        "DefaultVis",
    ]),
    entry(
        "OPTION",
        "-fvisibility=",
        ["fvisibility_EQ", "Joined", "f_Group", "INVALID", "0", "DefaultVis | CC1Option"],
        '"default,hidden,internal,protected"',
    ),
    entry("OPTION", "--visibility", [
        "_visibility",
        "Separate",
        "M_Group",
        "fvisibility_EQ",
        "0",
        "DefaultVis",
    ]),
    entry("OPTION", "-Wa,", ["Wa_COMMA", "CommaJoined", "INVALID", "INVALID", "0", "DefaultVis"]),
    entry("OPTION", "-MF", [
        "MF",
        "JoinedOrSeparate",
        "M_Group",
        "INVALID",
        "0",
        "DefaultVis | CC1Option",
    ]),
    "#endif // OPTION",
    "#ifdef CODEGEN_OPTION_WITH_MARSHALLING",
    entry(
        "CODEGEN_OPTION_WITH_MARSHALLING",
        "-fno-plt",
        ["fno_plt", "Flag", "f_Group", "INVALID", "0", "DefaultVis | CC1Option"],
        "nullptr",
        ", true, 0, CodeGenOpts.NoPLT, false, false, false, makeBooleanOptionNormalizer(true, false, OPT_fplt), makeBooleanOptionDenormalizer(true), -1",
    ),
    "#endif // CODEGEN_OPTION_WITH_MARSHALLING",
    "#ifdef LANG_OPTION_WITH_MARSHALLING",
    entry(
        "LANG_OPTION_WITH_MARSHALLING",
        "-fvisibility=",
        ["fvisibility_EQ", "Joined", "f_Group", "INVALID", "0", "DefaultVis | CC1Option"],
        '"default,hidden,internal,protected"',
        ", true, 0, LangOpts->ValueVisibilityMode, DefaultVisibility, false, 0, normalizeSimpleEnum, denormalizeSimpleEnum, 0",
    ),
    "#endif // LANG_OPTION_WITH_MARSHALLING",
    "#ifdef PREPROCESSOR_OUTPUT_OPTION_WITH_MARSHALLING",
    entry("PREPROCESSOR_OUTPUT_OPTION_WITH_MARSHALLING", "-MF", [
        "MF",
        "JoinedOrSeparate",
        "M_Group",
        "INVALID",
        "0",
        "DefaultVis | CC1Option",
    ]),
    "#endif // PREPROCESSOR_OUTPUT_OPTION_WITH_MARSHALLING",
].join("\n");

function option(options: ReadonlyMap<string, Option>, id: string): Option {
    const found = options.get(id);
    if (found === undefined) {
        throw new Error(`no option ${id}`);
    }
    return found;
}

describe("Options.inc", () => {
    const options = parseOptions(OPTIONS_INC);

    test("each OPTION entry gives an option, through its help text and its comment", () => {
        expect(options.size).toBe(11);
        expect(option(options, "fno_plt")).toEqual({
            id: "fno_plt",
            spelling: "-fno-plt",
            kind: "Flag",
            group: "f_Group",
            alias: "INVALID",
            flags: [],
            visibility: ["DefaultVis", "CC1Option"],
            values: undefined,
            marshalling: "CODEGEN",
        });
        expect(option(options, "Wa_COMMA").spelling).toBe("-Wa,");
        expect(option(options, "fident").flags).toEqual(["NoXarchOption", "Ignored"]);
        expect(option(options, "_visibility").alias).toBe("fvisibility_EQ");
    });

    test("an option takes the values its table lists and the kind of its marshalling", () => {
        expect(option(options, "fvisibility_EQ").values).toEqual([
            "default",
            "hidden",
            "internal",
            "protected",
        ]);
        expect(option(options, "fvisibility_EQ").marshalling).toBe("LANG");
        expect(option(options, "MF").marshalling).toBe("PREPROCESSOR_OUTPUT");
        expect(option(options, "fplt").marshalling).toBeUndefined();
    });

    test("a change of an option's entry against the previous table names its fields", () => {
        const previous = parseOptions(
            OPTIONS_INC.replace(
                "f_Group, INVALID, nullptr, 0, DefaultVis,",
                "INVALID, INVALID, nullptr, 0, DefaultVis,",
            ),
        );
        expect(change(option(options, "fplt"), previous.get("fplt"))).toBe(
            "changed group INVALID → f_Group",
        );
        expect(change(option(options, "fno_plt"), previous.get("fno_plt"))).toBeUndefined();
        expect(change(option(options, "fno_plt"), undefined)).toBe("new");
    });
});

describe("option_classes.inc", () => {
    const options = parseOptions(OPTIONS_INC);
    const classes = (table: string): Map<string, string> =>
        optionClasses(options, parseClassTable(table));

    test("what the driver ignores and what CodeGenOptions keeps start out left out", () => {
        const base = classes("");
        expect(base.get("fident")).toBe("LeftOut");
        expect(base.get("fno_plt")).toBe("LeftOut");
        expect(base.get("fplt")).toBe("Semantic");
        expect(base.get("fvisibility_EQ")).toBe("Semantic");
    });

    test("a group line covers the group's group chain, an alias by the option it aliases", () => {
        const table = classes("// The f group.\nOPTION_GROUP(CompileOnly_Group, Diagnostics)\n");
        expect(table.get("fident")).toBe("Diagnostics");
        expect(table.get("fno_plt")).toBe("Diagnostics");
        // --visibility is in M_Group, but aliases -fvisibility=, in f_Group.
        expect(table.get("_visibility")).toBe("Diagnostics");
        expect(table.get("MF")).toBe("Semantic");
        expect(table.get("Wa_COMMA")).toBe("Semantic");
    });

    test("a later line overrides an earlier one", () => {
        const table = classes(
            "OPTION_GROUP(f_Group, LeftOut)\nOPTION(fplt, Semantic)\n" +
                "OPTION(fvisibility_EQ, UserContent)\nOPTION_GROUP(clang_ignored_f_Group, Semantic)\n",
        );
        expect(table.get("fplt")).toBe("Semantic");
        expect(table.get("fno_plt")).toBe("LeftOut");
        expect(table.get("fvisibility_EQ")).toBe("UserContent");
        expect(table.get("fident")).toBe("Semantic");
    });

    test("a line naming an option the table lacks is an error", () => {
        expect(() => classes("OPTION(fgone, LeftOut)")).toThrow(/fgone/);
    });
});

describe("counterparts", () => {
    test("a negative spelling shares the spelling of its positive one", () => {
        expect(counterpartBase("-fno-plt")).toBe("-fplt");
        expect(counterpartBase("-mno-red-zone")).toBe("-mred-zone");
        expect(counterpartBase("-gno-column-info")).toBe("-gcolumn-info");
        expect(counterpartBase("--no-cuda-version-check")).toBe("--cuda-version-check");
        expect(counterpartBase("-no-pie")).toBe("-pie");
        expect(counterpartBase("/GR-")).toBe("/GR");
        expect(counterpartBase("-fnoplt")).toBe("-fnoplt");
        expect(counterpartBase("-Wa,")).toBe("-Wa,");
    });

    test("an option merges with its counterpart, apart from the rest", () => {
        const options = parseOptions(OPTIONS_INC);
        const members = ["fno_plt", "fplt", "fvisibility_EQ"].map((id) => ({
            option: option(options, id),
        }));
        expect(
            [...families(members)].map(([base, family]) => [
                base,
                family.map((each) => each.option.id),
            ]),
        ).toEqual([
            ["-fplt", ["fno_plt", "fplt"]],
            ["-fvisibility=", ["fvisibility_EQ"]],
        ]);
    });
});

describe("probes", () => {
    const options = parseOptions(OPTIONS_INC);

    test("the first job -### prints is the compile's", () => {
        const stderr =
            'clang version 23\n (in-process)\n "/bin/clang" "-cc1" "-MF" "a \\"b\\".d" "-fno-plt"\n "/bin/ld" "-o" "a"\n';
        expect(compileJob(stderr)).toEqual(["/bin/clang", "-cc1", "-MF", 'a "b".d', "-fno-plt"]);
        expect(compileJob("clang: error: unknown argument\n")).toBeUndefined();
    });

    test("a cc1 argument takes its value apart and the kind of its marshalling", () => {
        const table = cc1Table(options.values());
        expect(
            cc1Arguments(options, table, [
                "-MF",
                "deps.d",
                "-fvisibility=hidden",
                "-fno-plt",
                "-x",
            ]),
        ).toEqual([
            { text: "-MF deps.d", kind: "PREPROCESSOR_OUTPUT" },
            { text: "-fvisibility=hidden", kind: "LANG" },
            { text: "-fno-plt", kind: "CODEGEN" },
            { text: "-x", kind: "MANUAL:-x" },
        ]);
    });

    test("cc1 arguments differ as multisets", () => {
        const argument = (text: string) => ({ text, kind: "LANG" });
        expect(
            difference(["-a", "-a", "-b"].map(argument), ["-a", "-c"].map(argument)).map(
                ({ text }) => text,
            ),
        ).toEqual(["-a", "-b"]);
    });

    function evidence(fields: Partial<Evidence>): Evidence {
        return {
            accepted: ["x86"],
            macros: new Set(),
            features: new Set(),
            added: new Map(),
            removed: new Map(),
            rejection: undefined,
            ...fields,
        };
    }

    test("the bucket comes from what the option changes", () => {
        expect(bucket(evidence({ accepted: [] }))).toBe("B");
        expect(bucket(evidence({ macros: new Set(["+__PIC__ 2"]) }))).toBe("A");
        expect(bucket(evidence({ features: new Set(["-__has_feature(cxx_rtti)"]) }))).toBe("A");
        expect(bucket(evidence({}))).toBe("C");
        expect(bucket(evidence({ added: new Map([["-fno-plt", "CODEGEN"]]) }))).toBe("D");
        expect(bucket(evidence({ added: new Map([["-fms-extensions", "LANG"]]) }))).toBe("E");
        expect(bucket(evidence({ removed: new Map([["-fcolor-diagnostics", "DIAG"]]) }))).toBe("E");
    });

    function judged(cls: string, ...members: Partial<Evidence>[]): boolean {
        const found = finding(members.map(evidence));
        if (found === undefined) {
            throw new Error("no member was probed");
        }
        return suspicious(cls, found);
    }

    test("a family's evidence contradicts the class of each of its members", () => {
        const macro = { macros: new Set(["+_CPPRTTI 1"]) };
        const codegen = { added: new Map([["-fno-plt", "CODEGEN"]]) };
        const lang = { added: new Map([["-fms-extensions", "LANG"]]) };
        const diag = { removed: new Map([["-fcolor-diagnostics", "DIAG"]]) };
        expect(judged("LeftOut", macro)).toBe(true);
        expect(judged("LeftOut", lang)).toBe(true);
        expect(judged("LeftOut", diag, codegen)).toBe(false);
        expect(judged("LeftOut", codegen)).toBe(false);
        expect(judged("Semantic", codegen)).toBe(true);
        expect(judged("Semantic", {})).toBe(true);
        expect(judged("Diagnostics", codegen)).toBe(false);
        // The default spelling changes nothing; its counterpart does.
        expect(judged("Semantic", {}, macro)).toBe(false);
        expect(judged("LeftOut", {}, macro)).toBe(true);
        expect(finding([])).toBeUndefined();
    });
});
