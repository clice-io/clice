/// One-off check of the hover migration (tests/snap/hover → tests-next/hover):
/// every hover the old snapshots pinned is in the snap.toml of the case it
/// moved to, its markdown and range as they were, or listed below as
/// changed on purpose or dropped for a case that pins the same.
///
///   npx tsx tools/migrate/hover.ts

import {
    bare,
    corpus,
    newBlocks,
    oldBodies,
    oldSource,
    parseRange,
    renamed,
    renderOld,
    report,
    testOf,
    type Destination,
    Mapping,
    type Explained,
    type NewBlock,
    type Tally,
} from "./s_hover_shared.ts";

const H = "hover/";

/// Where each old fixture went; cases are under tests-next/hover.
const MAP: Record<string, Destination> = {
    "auto.cpp": {
        cases: ["deduced_types/04_auto_forms"],
        // The documented case pins the same forms.
        dropped: {
            "01_structured_bindings": "deduced_types/01_auto",
            "02_undeduced_auto": "deduced_types/01_auto",
            "04_auto_lambda": "deduced_types/01_auto",
            "05_auto_instantiation": "deduced_types/01_auto",
            "07_simple_auto": "deduced_types/01_auto",
            "08_const_auto": "deduced_types/01_auto",
            "10_auto_ref": "deduced_types/01_auto",
            "11_auto_ptr": "deduced_types/01_auto",
            "12_auto_from_pointer": "deduced_types/01_auto",
            "15_auto_trailing_return": "deduced_types/01_auto",
            "17_auto_fn_return": "deduced_types/01_auto",
            "25_undeduced_auto_decl": "deduced_types/01_auto",
        },
    },
    "basics.cpp": {
        cases: ["symbols/11_scope_lines", "layout/04_padding"],
        renames: {
            "03_field": "field",
            "07_method_local": "method_local",
        },
        dropped: {
            "10_predefined_var": "variables/06_predefined_identifiers",
            "11_predefined_var_dependent": "variables/06_predefined_identifiers",
        },
    },
    "c_records.cpp": {
        cases: ["members/06_c_records"],
        renames: { c_struct: "struct", c_mentions: "mentions", c_inner_tags: "inner_tags" },
    },
    "callee_args.cpp": {
        cases: ["arguments/05_argument_forms"],
        dropped: { "01_arg_by_ref": "arguments/01_bound_parameters" },
    },
    "concepts.cpp": { cases: ["templates/05_concepts"] },
    "decltype.cpp": {
        cases: ["deduced_types/05_decltype_forms"],
        dropped: {
            "01_decltype_auto": "deduced_types/02_decltype",
            "02_const_decltype_auto": "deduced_types/02_decltype",
            "04_ref_decltype_auto": "deduced_types/02_decltype",
            "07_decltype_lvalue": "deduced_types/02_decltype",
            "09_decltype_paren_lvalue": "deduced_types/02_decltype",
            "10_decltype_rvalue_ref": "deduced_types/02_decltype",
            "14_decltype_dependent": "deduced_types/02_decltype",
            "15_undeduced_decltype_auto": "deduced_types/02_decltype",
            "16_var_decltype_type": "deduced_types/02_decltype",
        },
    },
    "delayed_template_parsing/main.cpp": { cases: ["documentation/17_delayed_template_parsing"] },
    "docs.cpp": {
        cases: ["documentation/15_template_docs"],
        dropped: {
            "07_primary_doc": "documentation/01_doc_comments",
            "09_partial_spec_doc": "documentation/01_doc_comments",
            "10_primary_ref_doc": "documentation/01_doc_comments",
            "12_partial_spec_ref_doc": "documentation/01_doc_comments",
        },
    },
    "documentation/01_doxygen_comments.cpp": { cases: ["documentation/01_doc_comments"] },
    "documentation/02_accessor_docs.cpp": { cases: ["documentation/02_accessors"] },
    "documentation/03_copydoc.cpp": { cases: ["documentation/03_copydoc"] },
    "documentation/04_inherit_overridden_docs.cpp": { cases: ["documentation/04_overrides"] },
    "documentation/05_overload_doc_sharing.cpp": { cases: ["documentation/05_overloads"] },
    "documentation/07_comment_association.cpp": { cases: ["documentation/07_banner_comments"] },
    "documentation/08_decl_vs_def_docs.cpp": {
        cases: ["documentation/08_declaration_and_definition"],
        test: "declaration first",
    },
    "documentation/09_whitespace_preserve.cpp": { cases: ["documentation/09_markdown_tables"] },
    "documentation/10_comment_indentation.cpp": { cases: ["documentation/10_indented_blocks"] },
    "documentation/11_macro_template_doc.cpp": { cases: ["documentation/14_macro_templates"] },
    "enum_members.cpp": { cases: ["members/04_enumerators"] },
    "enum_value_wide.cpp": { cases: ["members/05_wide_enumerators"] },
    "expression_context/01_constant_value.cpp": { cases: ["variables/02_constant_values"] },
    "expression_context/02_callee_arguments.cpp": { cases: ["arguments/01_bound_parameters"] },
    "expression_context/03_pass_semantics.cpp": { cases: ["arguments/02_pass_modes"] },
    "expression_context/04_implicit_conversion.cpp": { cases: ["arguments/03_conversions"] },
    "expression_context/05_string_length.cpp": { cases: ["expressions/01_literals"] },
    "expression_context/07_record_value_misleading.cpp": {
        cases: ["arguments/04_record_arguments"],
    },
    "expressions.cpp": { cases: ["expressions/05_literal_forms"] },
    "fields.cpp": { cases: ["members/07_member_uses"] },
    "functions.cpp": { cases: ["functions/02_function_uses"] },
    "getter_setter.cpp": { cases: ["documentation/16_accessor_forms"] },
    "hover_correctness/01_ms_inheritance.cpp": { cases: ["layout/05_msvc_records"] },
    "hover_correctness/02_object_vs_function.cpp": { cases: ["symbols/07_object_or_function"] },
    "hover_correctness/03_large_enum_value.cpp": { cases: ["members/05_wide_enumerators"] },
    "hover_correctness/04_default_args_call.cpp": { cases: ["functions/02_function_uses"] },
    "hover_correctness/05_macro_shadowed_symbol.cpp": { cases: ["macros/07_shadowed_functions"] },
    "initializer_token_limit.cpp": { cases: ["variables/05_initializer_limits"] },
    "internal_partition/main.cpp": { cases: ["modules/03_internal_partitions"] },
    "lambdas.cpp": { cases: ["functions/03_lambdas"] },
    "layout_information/01_field_layout.cpp": { cases: ["layout/01_field_layout"] },
    "layout_information/02_type_layout.cpp": { cases: ["layout/02_type_layout"] },
    "layout_information/03_vtable_offset.cpp": { cases: ["layout/03_vtable_slots"] },
    "long_initializers/main.cpp": { cases: ["variables/05_initializer_limits"] },
    "macro_hover/01_macro_definition_sites.cpp": { cases: ["macros/02_definition_sites"] },
    "macro_hover/02_expansion_preview.cpp": { cases: ["macros/03_expansion_preview"] },
    "macro_hover/03_cli_macros.cpp": { cases: ["macros/04_command_line"] },
    "macro_hover/04_nested_arg_expansion.cpp": { cases: ["macros/05_nested_arguments"] },
    "macro_hover/05_expansion_before_definition.cpp": {
        cases: ["macros/01_use_before_definition"],
        renames: { "01_before_def": "before", "02_after_def": "after" },
    },
    "macros.cpp": {
        cases: ["macros/08_macro_forms"],
        dropped: {
            "01_object_def": "macros/02_definition_sites",
            "03_object_use": "macros/02_definition_sites",
            "06_ifdef_use": "macros/02_definition_sites",
            "07_undef_site": "macros/02_definition_sites",
            "10_cli_use": "macros/04_command_line",
            "15_nested_arg": "macros/05_nested_arguments",
        },
    },
    "member_truncation.cpp": { cases: ["members/03_member_limits"] },
    "misc.cpp": {
        cases: [
            "symbols/08_named_uses",
            "aliases/04_alias_chains",
            "symbols/12_labels",
            "expressions/04_operators",
        ],
        dropped: { "03_big_initializer": "variables/04_long_initializers" },
    },
    "module_related/02_module_name_hover/main.cpp": { cases: ["modules/01_module_names"] },
    "module_related/03_export_status.cpp": { cases: ["modules/02_export_status"] },
    "multi_token_names.cpp": { cases: ["symbols/09_operator_names"] },
    "no_hover.cpp": { cases: ["no_card/02_no_card_places", "expressions/05_literal_forms"] },
    "no_hover_errors.cpp": { cases: ["no_card/03_invalid_initializers"] },
    "pass_types.cpp": {
        cases: ["arguments/06_pass_types"],
        dropped: {
            "01_int_value_var": "arguments/02_pass_modes",
            "03_int_ref_var": "arguments/02_pass_modes",
            "04_int_const_ref_var": "arguments/02_pass_modes",
        },
    },
    "presentation/01_presentation.cpp": { cases: ["presentation/01_plain_text"] },
    "record_members.cpp": { cases: ["members/02_record_members"] },
    "spaceship.cpp": { cases: ["expressions/04_operators"] },
    "special_hover_targets/01_type_members_on_hover.cpp": { cases: ["members/01_type_members"] },
    "special_hover_targets/02_typedef_underlying.cpp": { cases: ["aliases/02_alias_targets"] },
    "special_hover_targets/04_attributes.cpp": { cases: ["attributes/01_attributes"] },
    "special_hover_targets/05_include_hover.cpp": { cases: ["directives/01_include_paths"] },
    "special_hover_targets/06_this_hover.cpp": { cases: ["expressions/02_this"] },
    "special_hover_targets/07_predefined_identifiers.cpp": {
        cases: ["variables/06_predefined_identifiers"],
    },
    "special_hover_targets/08_no_hover_negatives.cpp": { cases: ["no_card/01_meaningless_tokens"] },
    "symbol_information/01_qualified_name.cpp": { cases: ["symbols/02_scopes"] },
    "symbol_information/02_symbol_kind.cpp": { cases: ["symbols/01_symbol_kinds"] },
    "symbol_information/03_access_specifier.cpp": { cases: ["symbols/03_access"] },
    "symbol_information/04_definition_rendering.cpp": { cases: ["symbols/04_definitions"] },
    "symbol_information/05_initializer_truncation.cpp": {
        cases: ["variables/04_long_initializers"],
    },
    "symbol_information/06_virtual_modifiers.cpp": { cases: ["symbols/05_virtual_modifiers"] },
    "symbol_information/07_anon_namespace_scope.cpp": {
        cases: ["symbols/06_anonymous_namespaces"],
    },
    "tag_decls.cpp": { cases: ["symbols/08_named_uses"] },
    "templates.cpp": {
        cases: [
            "templates/06_template_forms",
            "templates/07_dependent_names",
            "templates/08_explicit_instantiations",
            "symbols/09_operator_names",
            "documentation/15_template_docs",
        ],
    },
    "this_expr.cpp": {
        cases: ["expressions/03_this_forms"],
        dropped: {
            "01_this_class": "expressions/02_this",
            "02_this_template_class": "expressions/02_this",
        },
    },
    "type_information/01_variable_type.cpp": {
        cases: ["variables/01_declarators"],
        renames: { "01_pointer": "pointer", "02_reference": "reference", "03_array": "array" },
    },
    "type_information/02_aka_desugar.cpp": { cases: ["aliases/01_aka"] },
    "type_information/03_function_signature.cpp": { cases: ["functions/01_signatures"] },
    "type_information/04_template_params.cpp": { cases: ["templates/01_template_parameters"] },
    "type_information/05_auto_deduction.cpp": { cases: ["deduced_types/01_auto"] },
    "type_information/06_decltype_deduction.cpp": { cases: ["deduced_types/02_decltype"] },
    "type_information/07_ctad_arguments.cpp": { cases: ["templates/03_deduced_class_arguments"] },
    "type_information/08_instantiation_args.cpp": {
        cases: ["templates/02_instantiation_arguments"],
    },
    "type_information/10_sugared_auto.cpp": { cases: ["deduced_types/03_alias_spelling"] },
    "type_information/12_c_typedef_anon.cpp": { cases: ["aliases/03_c_anonymous_structs"] },
    "type_information/13_concept_constraints.cpp": { cases: ["templates/04_concept_constraints"] },
    "unvisited_names.cpp": {
        cases: ["attributes/03_lambda_attributes", "expressions/05_literal_forms"],
    },
    "using_decls.cpp": { cases: ["symbols/10_using_declarations"] },
    "values.cpp": {
        cases: ["variables/03_values"],
        dropped: {
            "01_constexpr_call_value": "variables/02_constant_values",
            "02_sizeof_value": "variables/02_constant_values",
            "03_template_member_value": "variables/02_constant_values",
            "04_enumerator_value": "variables/02_constant_values",
        },
    },
    "variables.cpp": { cases: ["symbols/11_scope_lines"] },
};

/// Old facts the new output pins otherwise on purpose.
const EXPLAINED: Explained = {
    // Ported before this migration with names of its own and a const
    // reference: the same three declarators.
    "type_information/01_variable_type.cpp#01_pointer": "variables/01_declarators renamed",
    "type_information/01_variable_type.cpp#02_reference": "variables/01_declarators, const &",
    "type_information/01_variable_type.cpp#03_array": "variables/01_declarators renamed",
    // Ported before this migration as LIMIT 8; its no-card half matched.
    "macro_hover/05_expansion_before_definition.cpp#02_after_def": "macros/01 renamed",
    // The old card carried the fixture's own `// - flags:` line as the
    // struct's documentation: a comment atop a file attaches to its first
    // declaration (pinned in documentation/07_banner_comments). The case
    // keeps its note in test.ts, so the card shows none.
    "c_records.cpp#c_struct": "members/06_c_records without the stray comment",
};

interface OldHover {
    marker: string;
    range?: string;
    markdown?: string;
}

/// The hovers of one old snapshot body: `name: { range: "…" }` or
/// `name: NO HOVER` heads, each followed by its markdown.
function parseHovers(lines: string[], markers: ReadonlySet<string>): OldHover[] {
    const out: OldHover[] = [];
    let current: OldHover | undefined;
    let text: string[] = [];
    const flush = (): void => {
        if (current === undefined) {
            return;
        }
        while (text.length > 0 && text[text.length - 1] === "") {
            text.pop();
        }
        if (current.markdown === undefined && text.length > 0) {
            current.markdown = text.map((line) => line.trimEnd()).join("\n");
        }
        out.push(current);
    };
    for (const line of lines) {
        const head = /^([A-Za-z0-9_]+):(?: \{ range: "([^"]+)" \}| (NO HOVER))?$/.exec(line);
        if (head?.[1] !== undefined && markers.has(head[1])) {
            flush();
            text = [];
            if (head[3] !== undefined) {
                out.push({ marker: head[1] });
                current = undefined;
            } else {
                current = { marker: head[1], ...(head[2] === undefined ? {} : { range: head[2] }) };
            }
            continue;
        }
        text.push(line);
    }
    flush();
    return out;
}

/// The markdown an old card had, with paths the layout moved rewritten.
function normalize(markdown: string): string {
    return markdown.replace(/\$\{WS\}\/module_related\/02_module_name_hover\//g, "${WS}/");
}

function main(): void {
    const c = corpus("hover");
    const blocks = newBlocks("hover");
    const tally: Tally = { checked: 0, explained: 0, dropped: 0, unexplained: [] };
    const mapping = new Mapping();
    let oldAsked = 0;
    for (const fixture of c.fixtures) {
        if (!fixture.active) {
            continue;
        }
        const destination = MAP[fixture.rel];
        if (destination === undefined) {
            tally.unexplained.push(`${fixture.rel}: not mapped`);
            continue;
        }
        const paths = fixture.meta.verify === "both" ? 2 : 1;
        for (const body of oldBodies(c, fixture)) {
            const file = fixture.files.find((f) => f.rel.endsWith(body.file));
            const markers = new Set(file === undefined ? [] : [...file.source.offsets.keys()]);
            const source = oldSource(fixture, body.file);
            for (const hover of parseHovers(body.lines, markers)) {
                // A separate fixture's two snapshots are one question each.
                oldAsked += body.variant === "inspect" || body.variant === "server" ? 1 : paths;
                const key = `${fixture.rel}#${hover.marker}${body.variant === "" ? "" : `@${body.variant}`}`;
                const dropped = destination.dropped?.[hover.marker];
                if (dropped !== undefined) {
                    tally.dropped += 1;
                    mapping.record(fixture.rel, hover.marker, `dropped: ${dropped} pins the same`);
                    continue;
                }
                const label = destination.renames?.[hover.marker] ?? renamed(hover.marker);
                const { test, client } = testOf(body.variant);
                const found: NewBlock[] = destination.cases.flatMap((id) =>
                    (blocks.get(H + id) ?? []).filter(
                        (b) =>
                            b.label === label &&
                            b.test === (destination.test ?? test) &&
                            b.client === client,
                    ),
                );
                const [block] = found;
                if (block === undefined || found.length !== 1) {
                    tally.unexplained.push(
                        `${key}: ${found.length} blocks named ${label} in ${destination.cases.join(", ")}`,
                    );
                    continue;
                }
                const renamedTo =
                    label === renamed(hover.marker) ? "" : ` (${hover.marker} → ${label})`;
                const under = block.test === "answers" ? "" : ` [${block.test}]`;
                mapping.record(fixture.rel, hover.marker, `${block.caseId}${under}${renamedTo}`);
                const card = block.table["hover"] as Record<string, unknown> | undefined;
                const want = {
                    markdown: hover.markdown === undefined ? undefined : normalize(hover.markdown),
                    range:
                        hover.range === undefined
                            ? undefined
                            : renderOld(source, parseRange(hover.range)),
                };
                const got = {
                    markdown: card?.["markdown"] as string | undefined,
                    range: typeof card?.["range"] === "string" ? bare(card["range"]) : undefined,
                };
                if (want.markdown === got.markdown && want.range === got.range) {
                    tally.checked += 1;
                    continue;
                }
                const why = EXPLAINED[key];
                if (why !== undefined) {
                    tally.explained += 1;
                    continue;
                }
                tally.unexplained.push(
                    `${key} → ${block.caseId} [${block.test}.${label}]:\n` +
                        `      old range ${String(want.range)}, new ${String(got.range)}\n` +
                        `      old markdown ${JSON.stringify(want.markdown)}\n` +
                        `      new markdown ${JSON.stringify(got.markdown)}`,
                );
            }
        }
    }
    let newAsked = 0;
    for (const [id, list] of blocks) {
        const single = SINGLE_PATH.has(id.slice(H.length));
        for (const block of list) {
            if ("hover" in block.table) {
                newAsked += block.client !== undefined || single ? 1 : 2;
            }
        }
    }
    report("hover", tally, oldAsked, newAsked);
    mapping.print("old hover fixture");
}

/// Cases one client alone answers (their test.ts says why).
const SINGLE_PATH = new Set([
    "modules/01_module_names",
    "modules/03_internal_partitions",
    // Scenario tests on a server, one per hover order.
    "documentation/08_declaration_and_definition",
]);

main();
