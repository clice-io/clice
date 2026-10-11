/// One-off check of the semantic_tokens migration: every token an old
/// snapshot pinned is in the case it moved to. A whole-file snapshot's
/// tokens compare rendered the way the new output names them; a marker's
/// token compares by the line the marker sits on and what the token is,
/// since the case names its markers anew.
///
///     node tools/migrate/semantic_tokens.ts

import { scanFixtureHeader } from "../snap/corpus.ts";
import {
    asked,
    bare,
    caseSource,
    corpus,
    newBlocks,
    oldBody,
    oldSource,
    position,
    render,
    Tally,
    topicCases,
} from "./s_tokens_shared.ts";

const MOVED: Record<string, string> = {
    "attributes/01_attributes.cpp": "lexical/09_attributes",
    "braced_macro_argument.cpp": "preprocessor/06_braced_macro_arguments",
    "conflict_ambiguity/01_conflict_using.cpp": "ambiguity/01_conflicts",
    "conflict_ambiguity/02_conflict_type_variable.cpp": "ambiguity/01_conflicts",
    "conflict_ambiguity/03_using_overloads.cpp": "ambiguity/02_overload_sets",
    "conflict_ambiguity/04_injected_class_name.cpp": "ambiguity/03_injected_class_names",
    "declaration_tokens.cpp": "declarations/20_declaration_canary",
    "declarations/01_namespaces.cpp": "declarations/02_namespaces",
    "declarations/02_types.cpp": "declarations/03_types",
    "declarations/03_functions.cpp": "declarations/04_functions",
    "declarations/04_variables.cpp": "declarations/05_variables",
    "declarations/05_templates.cpp": "declarations/06_templates",
    "declarations/06_concepts.cpp": "declarations/07_concepts",
    "declarations/07_labels.cpp": "declarations/08_labels",
    "declarations/08_structured_bindings.cpp": "declarations/09_structured_bindings",
    "declarations/09_lambda_init_capture.cpp": "declarations/10_lambda_init_captures",
    "declarations/10_deduction_guides.cpp": "declarations/11_deduction_guides",
    "declarations/11_explicit_instantiation_class.cpp": "declarations/12_explicit_instantiations",
    "declarations/12_variable_templates.cpp": "declarations/13_variable_templates",
    "declarations/13_out_of_line_methods.cpp": "declarations/14_out_of_line_members",
    "declarations/14_alias_templates.cpp": "declarations/15_alias_templates",
    "declarations/15_template_template_params.cpp": "declarations/16_template_template_parameters",
    "declarations/16_friend_declarations.cpp": "declarations/17_friend_declarations",
    "declarations/17_explicit_instantiation_function.cpp":
        "declarations/12_explicit_instantiations",
    "declarations/18_explicit_instantiation_variable.cpp":
        "declarations/12_explicit_instantiations",
    "declarations/19_explicit_instantiation_member_bodies.cpp":
        "references/10_instantiated_members",
    "expression_tokens.cpp": "references/11_expression_canary",
    "global_anonymous_union.cpp": "declarations/19_anonymous_unions",
    "internal_partition.cpp": "modules/04_internal_partitions",
    "lexical_tokens/01_comments.cpp": "lexical/01_comments",
    "lexical_tokens/02_literals.cpp": "lexical/02_literals",
    "lexical_tokens/03_keywords.cpp": "lexical/03_keywords",
    "lexical_tokens/04_directives.cpp": "preprocessor/01_directives",
    "lexical_tokens/05_inactive_regions.cpp": "preprocessor/03_inactive_branches",
    "lexical_tokens/06_include_names.cpp": "preprocessor/02_header_names",
    "lexical_tokens/07_inactive_preamble.cpp": "preamble/01_inactive_preamble",
    "lexical_tokens/08_literal_affixes.cpp": "lexical/05_literal_affixes",
    "lexical_tokens/09_escape_sequences.cpp": "lexical/06_escape_sequences",
    "lexical_tokens/10_declarator_operators.cpp": "lexical/07_declarator_operators",
    "lexical_tokens/11_primitive_types.cpp": "lexical/04_primitive_types",
    "lexical_tokens/12_bracket_pairs.cpp": "lexical/08_brackets",
    "macros/01_macro.cpp": "preamble/02_preamble_macros",
    "macros/02_macro_expansion.cpp": "preprocessor/04_macro_expansions",
    "macros/03_macro_kinds.cpp": "preprocessor/05_macro_kinds",
    "modules/01_modules.cpp": "modules/01_module_declarations",
    "modules/02_module_partition.cpp": "modules/02_partitions",
    "modules/03_module_keyword_identifier.cpp": "modules/03_contextual_keywords",
    "pragma_clang_debug.cpp": "preprocessor/07_debug_pragmas",
    "references/01_member_init_list.cpp": "references/01_member_initializers",
    "references/02_using_declarations.cpp": "references/02_using_declarations",
    "references/03_sizeof_pack.cpp": "references/03_sizeof_packs",
    "references/04_using_enum.cpp": "references/04_using_enum",
    "references/05_dependent_names.cpp": "references/05_dependent_names",
    "references/06_lambda_captures.cpp": "references/06_lambda_captures",
    "references/07_range_for.cpp": "references/07_range_for",
    "references/08_enum_base.cpp": "references/08_enum_bases",
    "references/09_dependent_using.cpp": "references/09_dependent_using",
    "token_correctness/01_constructors_destructors.cpp":
        "special_members/01_constructors_destructors",
    "token_correctness/02_anonymous_parameters.cpp": "declarations/18_unnamed_parameters",
    "token_correctness/03_operator_names.cpp": "special_members/02_operator_names",
    "token_correctness/04_template_destructor.cpp": "special_members/04_template_destructors",
    "token_correctness/05_conversion_operators.cpp": "special_members/03_conversion_operators",
    "token_correctness/06_pseudo_destructor.cpp": "special_members/05_pseudo_destructors",
    "token_correctness/07_defaulted_deleted.cpp": "special_members/06_defaulted_members",
    "token_modifiers/01_decl_def_modifiers.cpp": "modifiers/01_declaration_definition",
    "token_modifiers/02_static_modifier.cpp": "modifiers/02_static",
    "token_modifiers/03_readonly_modifier.cpp": "modifiers/03_readonly",
    "token_modifiers/04_virtual_abstract.cpp": "modifiers/04_virtual_abstract",
    "token_modifiers/05_deprecated_modifier.cpp": "modifiers/05_deprecated",
    "token_modifiers/06_default_library.cpp": "modifiers/06_default_library",
    "token_modifiers/07_scope_modifiers.cpp": "modifiers/07_scopes",
    "token_modifiers/08_mutable_reference.cpp": "modifiers/08_mutable_arguments",
    "token_modifiers/09_deduced_modifier.cpp": "modifiers/09_deduced_types",
    "token_modifiers/10_user_defined_operator.cpp": "modifiers/10_overloaded_operators",
};

const TOKEN =
    /^- \{ loc: "(\d+:\d+)", text: (".*"|[^,]*), kind: (\w+)(?:, modifiers: \[([^\]]*)\])? \}$/;

interface Token {
    at: string;
    text: string;
    type: string;
    modifiers?: string[];
}

function describe(type: string, modifiers: readonly string[]): string {
    return modifiers.length === 0 ? type : `${type} [${modifiers.join(", ")}]`;
}

function read(text: string): string {
    return text.startsWith('"') ? (JSON.parse(text) as string) : text;
}

const tally = new Tally();
const c = corpus("semantic_tokens");
for (const fixture of c.fixtures) {
    const moved = MOVED[fixture.rel];
    if (moved === undefined) {
        tally.unexplained.push(`${fixture.rel}: no case`);
        continue;
    }
    if (!fixture.active) {
        continue;
    }
    const id = `semantic_tokens/${moved}`;
    const entry = fixture.files.find((file) => file.rel === fixture.rel);
    const focused =
        (entry?.source.offsets.size ?? 0) + (entry?.source.namelessOffsets.length ?? 0) > 0;
    const header = scanFixtureHeader(entry?.content ?? "");
    const source = oldSource(fixture);
    const { source: now, markers } = caseSource(id);
    const variants =
        fixture.meta.snap === "separate" ? (["inspect", "server"] as const) : ([""] as const);
    for (const variant of variants) {
        const old: string[] = [];
        const gone = new Set<string>();
        for (const line of oldBody(c, fixture, variant).get(source.path) ?? []) {
            const match = TOKEN.exec(line);
            if (match === null) {
                throw new Error(`${fixture.rel}: unread line ${line}`);
            }
            const start = position(match[1] ?? "");
            const text = read(match[2] ?? "");
            const modifiers = (match[4] ?? "").split(", ").filter((m) => m !== "");
            const kind = match[3] ?? "";
            if (focused) {
                old.push(
                    `${source.line(start.line).trim()} | ${text} ${describe(kind, modifiers)}`,
                );
                continue;
            }
            const end = { line: start.line, character: start.character + text.length };
            const fact = `${render(source, { start, end })} ${describe(kind, modifiers)}`;
            old.push(fact);
            // The old doc header, its `// snap:` notes and `// - key:` lines
            // are not in the case.
            if (kind === "comment" && (start.line < header.bodyStart || !now.text.includes(text))) {
                gone.add(fact);
            }
        }
        tally.oldAsked += focused ? old.length * (variant === "" ? 2 : 1) : variant === "" ? 2 : 1;
        const test = variant === "" ? "answers" : `answers.${variant}`;
        const found: string[] = [];
        for (const [label, block] of newBlocks(id).get(test) ?? []) {
            const token = block["semantic_token"] as Token | undefined;
            if (token !== undefined) {
                const at = markers.get(label);
                if (at === undefined) {
                    throw new Error(`${id}: no marker ${label}`);
                }
                found.push(
                    `${now.line(at.line).trim()} | ${token.text} ${describe(token.type, token.modifiers ?? [])}`,
                );
            }
            for (const piece of (block["semantic_tokens"] ?? []) as Token[]) {
                found.push(`${bare(piece.at)} ${describe(piece.type, piece.modifiers ?? [])}`);
            }
        }
        tally.check(`${fixture.rel}${variant === "" ? "" : ` (${variant})`}`, old, found, (fact) =>
            gone.has(fact) ? "a comment the case no longer holds" : undefined,
        );
    }
}
for (const id of topicCases("semantic_tokens")) {
    tally.newAsked += asked(id);
}
tally.print("semantic_tokens");
