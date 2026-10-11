/// One-off check of the navigation snap corpus migration (S-NAV): for every
/// request a new case asks, each entry the old snapshot pinned for it is in
/// the case's snap.toml, rendered the way the new output names it, and the
/// new block holds nothing the old one did not, unless EXPLAINED says why.
/// The old suite asked every marker every navigation request; a case now
/// asks only what its section tests (board decision d5, ykiko 10-11), and
/// the entries of the requests it no longer asks are counted, not compared.
/// A dropped fixture names the case that pins the same. Run it from the
/// repository: `node tools/migrate/navigation.ts`.
///
/// Hierarchy items render by name, kind and the place of the name; the old
/// snapshots also pinned each item's whole extent, which no editor shows
/// and the new output leaves out.

import type { SnapFixture } from "../snap/corpus.ts";
import type { Table } from "../testing/render/value.ts";
import {
    corpus,
    missing,
    newBlocks,
    oldBlocks,
    OldFiles,
    stripOffset,
    type OldBlock,
} from "./s_nav_shared.ts";

interface Step {
    /// The fixture, corpus-relative.
    old: string;
    /// The navigation case it moved to, or the case pinning the same.
    to: string;
    /// doc and reg: a case of its own, with and without a page; merge: folded
    /// into another fixture's case; existing: a case already there took it;
    /// drop: `to` pins the same.
    kind: "doc" | "reg" | "drop" | "merge" | "existing";
    why?: string;
    /// Old marker → new marker, where renamed.
    rename?: Record<string, string>;
}

const PLAN: Step[] = [
    // definition
    {
        old: "go_to_definition/03_def_declaration_only.cpp",
        to: "definition/01_declaration_only",
        kind: "existing",
        rename: {
            pure_use: "method",
            fn_use: "function",
            var_use: "variable",
            const_use: "constant",
        },
    },
    {
        old: "go_to_definition/02_def_decl_alternate.cpp",
        to: "definition/03_declaration_and_definition",
        kind: "doc",
    },
    {
        old: "decl_def_alternate.cpp",
        to: "definition/03_declaration_and_definition",
        kind: "merge",
        why: "the same code and markers as go_to_definition/02_def_decl_alternate",
    },
    {
        old: "inline_definition.cpp",
        to: "definition/03_declaration_and_definition",
        kind: "merge",
        why: "the inline function its prose describes",
        rename: { def: "inline_def", use: "inline_use" },
    },
    { old: "declaration_only.cpp", to: "definition/04_declaration_sites", kind: "reg" },
    {
        old: "go_to_definition/04_def_include/main.cpp",
        to: "definition/05_include_directives",
        kind: "doc",
    },
    {
        old: "go_to_definition/05_def_local_symbol.cpp",
        to: "definition/06_local_symbols",
        kind: "doc",
    },
    {
        old: "go_to_definition/09_def_error_recovery.cpp",
        to: "definition/07_error_recovery",
        kind: "doc",
    },
    // projects (indexing)
    {
        old: "go_to_definition/01_def_cross_tu/main.cpp",
        to: "projects/01_header_to_source",
        kind: "drop",
        why: "the same layout: a use, the prototype in a header, the definition in a sibling source",
    },
    {
        old: "go_to_declaration/01_decl_cross_tu/main.cpp",
        to: "projects/01_header_to_source",
        kind: "drop",
        why: "the same layout: a use, the prototype in a header, the definition in a sibling source",
    },
    {
        old: "find_references/01_refs_cross_tu/main.cpp",
        to: "projects/03_uses_across_sources",
        kind: "doc",
    },
    { old: "header_source/main.cpp", to: "projects/04_header_and_sources", kind: "reg" },
    { old: "internal_across_units/main.cpp", to: "projects/05_internal_linkage", kind: "reg" },
    // declaration
    {
        old: "go_to_declaration/02_decl_function_prototype.cpp",
        to: "declaration/01_functions",
        kind: "doc",
    },
    {
        old: "go_to_declaration/03_decl_forward_class.cpp",
        to: "declaration/02_forward_declarations",
        kind: "doc",
    },
    {
        old: "go_to_declaration/04_decl_static_member.cpp",
        to: "declaration/03_static_members",
        kind: "doc",
    },
    {
        old: "static_data_member.cpp",
        to: "declaration/03_static_members",
        kind: "drop",
        why: "the same shape: a static data member declared in its class, defined outside and used",
    },
    {
        old: "go_to_declaration/05_decl_extern_variable.cpp",
        to: "declaration/04_extern_variables",
        kind: "doc",
    },
    { old: "multi_declaration.cpp", to: "declaration/05_redeclarations", kind: "reg" },
    {
        old: "go_to_declaration/06_decl_multiple.cpp",
        to: "declaration/05_redeclarations",
        kind: "merge",
        why: "the same code as multi_declaration.cpp",
        rename: { first: "first_decl", second: "second_decl" },
    },
    {
        old: "go_to_declaration/07_decl_signature_mismatch.cpp",
        to: "declaration/06_signature_spelling",
        kind: "doc",
    },
    // deduced types
    {
        old: "go_to_definition/12_def_auto_keyword.cpp",
        to: "deduced_types/01_auto_keyword",
        kind: "doc",
    },
    {
        old: "go_to_definition/15_def_auto_declarators.cpp",
        to: "deduced_types/02_auto_declarators",
        kind: "doc",
    },
    { old: "go_to_definition/16_def_decltype.cpp", to: "deduced_types/03_decltype", kind: "doc" },
    {
        old: "go_to_definition/17_def_auto_templates.cpp",
        to: "deduced_types/04_deduced_templates",
        kind: "doc",
    },
    {
        old: "go_to_definition/18_def_auto_params_returns.cpp",
        to: "deduced_types/05_parameters_and_returns",
        kind: "doc",
    },
    {
        old: "go_to_definition/19_def_auto_dependent.cpp",
        to: "deduced_types/06_dependent_auto",
        kind: "doc",
    },
    // type definition
    {
        old: "go_to_type_definition/01_typedef_variables.cpp",
        to: "type_definition/01_variables",
        kind: "doc",
    },
    {
        old: "go_to_type_definition/02_typedef_field.cpp",
        to: "type_definition/02_fields",
        kind: "doc",
    },
    {
        old: "go_to_type_definition/03_typedef_auto.cpp",
        to: "type_definition/03_deduced_variables",
        kind: "doc",
    },
    {
        old: "go_to_type_definition/04_typedef_smart_pointer.cpp",
        to: "type_definition/04_smart_pointers",
        kind: "doc",
    },
    {
        old: "go_to_type_definition/05_typedef_alias.cpp",
        to: "type_definition/05_type_aliases",
        kind: "doc",
    },
    {
        old: "go_to_type_definition/06_typedef_structured_binding.cpp",
        to: "type_definition/06_structured_bindings",
        kind: "doc",
    },
    {
        old: "go_to_type_definition/07_typedef_pointer_reference.cpp",
        to: "type_definition/07_pointers_and_references",
        kind: "doc",
    },
    { old: "type_definition.cpp", to: "type_definition/08_aliased_variables", kind: "reg" },
    // implementation
    {
        old: "go_to_implementation/01_impl_virtual_chain.cpp",
        to: "implementation/01_override_chain",
        kind: "doc",
    },
    {
        old: "go_to_implementation/02_impl_virtual_siblings.cpp",
        to: "implementation/02_sibling_overrides",
        kind: "doc",
    },
    {
        old: "go_to_implementation/03_impl_nonvirtual_def.cpp",
        to: "implementation/03_non_virtual",
        kind: "doc",
    },
    {
        old: "go_to_implementation/04_impl_base_derived.cpp",
        to: "implementation/04_derived_classes",
        kind: "doc",
    },
    {
        old: "go_to_implementation/05_impl_template_duck_type.cpp",
        to: "implementation/05_dependent_calls",
        kind: "doc",
    },
    { old: "implementation_abstract.cpp", to: "implementation/06_pure_overrides", kind: "reg" },
    { old: "implementation_virtual.cpp", to: "implementation/07_virtual_calls", kind: "reg" },
    // references
    {
        old: "find_references/02_refs_include_declaration.cpp",
        to: "references/01_declarations_included",
        kind: "doc",
    },
    { old: "find_references/03_refs_range_for.cpp", to: "references/02_range_for", kind: "doc" },
    {
        old: "find_references/04_refs_implicit_construction.cpp",
        to: "references/03_implicit_construction",
        kind: "doc",
    },
    {
        old: "find_references/05_refs_forwarding.cpp",
        to: "references/04_forwarding_factories",
        kind: "doc",
    },
    {
        old: "find_references/06_refs_dependent_context.cpp",
        to: "references/05_dependent_calls",
        kind: "doc",
    },
    {
        old: "find_references/07_refs_read_write.cpp",
        to: "references/06_access_kinds",
        kind: "doc",
    },
    {
        old: "find_references/08_refs_enclosing_context.cpp",
        to: "references/07_enclosing_functions",
        kind: "doc",
    },
    { old: "find_references/11_refs_label_goto.cpp", to: "references/08_labels", kind: "doc" },
    { old: "overload_references.cpp", to: "references/09_overloads", kind: "reg" },
    { old: "weak_references.cpp", to: "references/10_template_uses", kind: "reg" },
    // call hierarchy
    {
        old: "call_hierarchy/01_calls_prepare.cpp",
        to: "call_hierarchy/01_functions_and_methods",
        kind: "doc",
    },
    {
        old: "call_hierarchy/02_calls_incoming.cpp",
        to: "call_hierarchy/02_incoming_calls",
        kind: "doc",
    },
    {
        old: "call_hierarchy/03_calls_outgoing.cpp",
        to: "call_hierarchy/03_outgoing_calls",
        kind: "doc",
    },
    {
        old: "call_hierarchy/04_calls_detail_signature.cpp",
        to: "call_hierarchy/04_signature_details",
        kind: "doc",
    },
    {
        old: "call_hierarchy/05_calls_qualified_name.cpp",
        to: "call_hierarchy/05_qualified_names",
        kind: "doc",
    },
    {
        old: "call_hierarchy/06_calls_virtual_dispatch.cpp",
        to: "call_hierarchy/06_virtual_dispatch",
        kind: "doc",
    },
    {
        old: "call_hierarchy/07_calls_non_function.cpp",
        to: "call_hierarchy/07_non_functions",
        kind: "doc",
    },
    {
        old: "call_hierarchy/08_calls_lambda.cpp",
        to: "call_hierarchy/08_lambda_bodies",
        kind: "doc",
    },
    {
        old: "call_hierarchy/09_calls_forwarding_ctor.cpp",
        to: "call_hierarchy/09_forwarding_factories",
        kind: "doc",
    },
    { old: "call_hierarchy.cpp", to: "call_hierarchy/10_call_chains", kind: "reg" },
    { old: "implicit_calls.cpp", to: "call_hierarchy/11_implicit_calls", kind: "reg" },
    { old: "included_body/main.cpp", to: "call_hierarchy/12_included_bodies", kind: "reg" },
    {
        old: "relation_only_symbol/main.cpp",
        to: "call_hierarchy/13_header_conversions",
        kind: "reg",
    },
    // type hierarchy
    { old: "type_hierarchy/01_types_prepare.cpp", to: "type_hierarchy/01_type_kinds", kind: "doc" },
    {
        old: "type_hierarchy/02_types_supertypes.cpp",
        to: "type_hierarchy/02_supertypes",
        kind: "doc",
    },
    { old: "type_hierarchy/03_types_subtypes.cpp", to: "type_hierarchy/03_subtypes", kind: "doc" },
    {
        old: "type_hierarchy/04_types_template_inheritance.cpp",
        to: "type_hierarchy/04_template_bases",
        kind: "doc",
    },
    {
        old: "type_hierarchy/05_types_template_args.cpp",
        to: "type_hierarchy/05_template_arguments",
        kind: "doc",
    },
    { old: "type_hierarchy.cpp", to: "type_hierarchy/06_class_trees", kind: "reg" },
    // implicit construction
    {
        old: "implicit_construction/01_constructor_call.cpp",
        to: "implicit_construction/01_constructor_calls",
        kind: "doc",
    },
    {
        old: "implicit_construction/02_copy_move.cpp",
        to: "implicit_construction/02_copy_and_move",
        kind: "doc",
    },
    {
        old: "implicit_construction/03_ctad.cpp",
        to: "implicit_construction/03_deduced_class_templates",
        kind: "doc",
    },
    {
        old: "implicit_construction/04_aggregate_init.cpp",
        to: "implicit_construction/04_aggregates",
        kind: "doc",
    },
    {
        old: "implicit_construction/05_member_init.cpp",
        to: "implicit_construction/05_member_initializers",
        kind: "doc",
    },
    {
        old: "implicit_construction/06_delegating_ctor.cpp",
        to: "implicit_construction/06_delegating_constructors",
        kind: "doc",
    },
    {
        old: "implicit_construction/07_inherited_ctor.cpp",
        to: "implicit_construction/07_inherited_constructors",
        kind: "doc",
    },
    {
        old: "implicit_construction/08_return_construction.cpp",
        to: "implicit_construction/08_return_values",
        kind: "doc",
    },
    {
        old: "implicit_construction/09_lambda_capture.cpp",
        to: "implicit_construction/09_lambda_captures",
        kind: "doc",
    },
    {
        old: "implicit_construction/10_structured_binding.cpp",
        to: "implicit_construction/10_structured_bindings",
        kind: "doc",
    },
    // implicit targets
    {
        old: "implicit_targets/01_override_final.cpp",
        to: "implicit_targets/01_override_and_final",
        kind: "doc",
    },
    {
        old: "implicit_targets/02_break_continue.cpp",
        to: "implicit_targets/02_break_and_continue",
        kind: "doc",
    },
    {
        old: "implicit_targets/03_delete_dtor.cpp",
        to: "implicit_targets/03_delete_expressions",
        kind: "doc",
    },
    {
        old: "implicit_targets/04_new_ctor.cpp",
        to: "implicit_targets/04_new_expressions",
        kind: "doc",
    },
    {
        old: "implicit_targets/05_operator_call.cpp",
        to: "implicit_targets/05_overloaded_operators",
        kind: "doc",
    },
    {
        old: "implicit_targets/06_rewritten_operator.cpp",
        to: "implicit_targets/06_rewritten_comparisons",
        kind: "doc",
    },
    {
        old: "implicit_targets/07_udl.cpp",
        to: "implicit_targets/07_user_defined_literals",
        kind: "doc",
    },
    {
        old: "implicit_targets/08_conversion_context.cpp",
        to: "implicit_targets/08_implicit_conversions",
        kind: "doc",
    },
    {
        old: "implicit_targets/09_cast_conversion.cpp",
        to: "implicit_targets/09_casts",
        kind: "doc",
    },
    { old: "implicit_targets/10_range_for.cpp", to: "implicit_targets/10_range_for", kind: "doc" },
    {
        old: "implicit_targets/11_coroutine.cpp",
        to: "implicit_targets/11_coroutine_keywords",
        kind: "doc",
    },
    // macros
    { old: "macro_sites.cpp", to: "macros/01_macro_arguments", kind: "reg" },
    {
        old: "go_to_definition/06_def_macro_wrapper.cpp",
        to: "macros/01_macro_arguments",
        kind: "merge",
        why: "the same code as macro_sites.cpp",
        rename: { decl: "macro_decl", def: "macro_def", use: "plain_use" },
    },
    { old: "macro_generated.cpp", to: "macros/02_generated_names", kind: "reg" },
    {
        old: "go_to_definition/07_def_macro_generated.cpp",
        to: "macros/02_generated_names",
        kind: "merge",
        why: "the same lines as macro_generated.cpp",
    },
    {
        old: "go_to_definition/08_def_macro_body.cpp",
        to: "macros/02_generated_names",
        kind: "drop",
        why: "body_name and counter_site pin the same: a token in a macro body navigates nowhere, the invocation to the macro",
    },
    { old: "find_references/09_refs_macro.cpp", to: "macros/04_macro_references", kind: "doc" },
    {
        old: "find_references/10_refs_macro_in_macro.cpp",
        to: "macros/05_nested_macros",
        kind: "doc",
    },
    { old: "macro_conditional.cpp", to: "macros/06_conditional_directives", kind: "reg" },
    { old: "macro_redefine.cpp", to: "macros/07_redefinitions", kind: "reg" },
    { old: "macro_header/main.cpp", to: "macros/08_header_macros", kind: "reg" },
    { old: "macro_paste_class.cpp", to: "macros/09_pasted_classes", kind: "reg" },
    // modules
    {
        old: "module_navigation/01_module_import_name/main.cpp",
        to: "modules/01_import_names",
        kind: "doc",
    },
    {
        old: "module_navigation/02_module_partition_import/main.cpp",
        to: "definition/02_partition_import",
        kind: "drop",
        why: "the same layout: a partition import and a use of what the partition exports",
        rename: { partition_ref: "partition" },
    },
    { old: "module_impl_unit/main.cpp", to: "modules/02_implementation_units", kind: "reg" },
    {
        old: "module_navigation/03_module_iface_impl/main.cpp",
        to: "modules/02_implementation_units",
        kind: "merge",
        why: "the same code as module_impl_unit",
        rename: { iface_name: "iface_module_name", impl_name: "impl_module_name" },
    },
    {
        old: "module_navigation/04_module_dotted/main.cpp",
        to: "modules/03_dotted_names",
        kind: "doc",
    },
    { old: "module_basic/main.cpp", to: "modules/04_exported_and_internal", kind: "reg" },
    { old: "module_partition/main.cpp", to: "modules/05_partition_units", kind: "reg" },
    // members
    { old: "constructor_navigation.cpp", to: "members/01_constructors", kind: "reg" },
    { old: "destructor_navigation.cpp", to: "members/02_destructors", kind: "reg" },
    { old: "conversion_operator.cpp", to: "members/03_conversion_operators", kind: "reg" },
    { old: "operator_overload.cpp", to: "members/04_operators", kind: "reg" },
    { old: "multi_token_names.cpp", to: "members/05_multi_token_names", kind: "reg" },
    { old: "nested_class.cpp", to: "members/06_nested_classes", kind: "reg" },
    { old: "inherited_member_access.cpp", to: "members/07_inherited_members", kind: "reg" },
    { old: "inherited_using.cpp", to: "members/08_using_declarations", kind: "reg" },
    { old: "friend_functions.cpp", to: "members/09_friends", kind: "reg" },
    // templates
    {
        old: "go_to_definition/10_def_dependent_type.cpp",
        to: "templates/01_dependent_members",
        kind: "doc",
    },
    {
        old: "go_to_definition/11_def_template_spec.cpp",
        to: "templates/02_explicit_specializations",
        kind: "doc",
    },
    {
        old: "go_to_definition/13_def_overload_candidates.cpp",
        to: "templates/03_overload_candidates",
        kind: "doc",
    },
    {
        old: "go_to_definition/14_def_dependent_expression.cpp",
        to: "templates/04_dependent_expressions",
        kind: "doc",
    },
    { old: "template_navigation.cpp", to: "templates/05_specializations", kind: "reg" },
    { old: "variable_template.cpp", to: "templates/06_variable_templates", kind: "reg" },
    { old: "concepts.cpp", to: "templates/07_concepts", kind: "reg" },
    { old: "ctad_guide.cpp", to: "templates/08_deduction_guides", kind: "reg" },
    { old: "dependent_names.cpp", to: "templates/09_dependent_names", kind: "reg" },
    // scopes
    { old: "anonymous_namespace.cpp", to: "scopes/01_anonymous_namespaces", kind: "reg" },
    { old: "shadowed_names.cpp", to: "scopes/02_shadowed_names", kind: "reg" },
    { old: "using_alias.cpp", to: "scopes/03_namespace_aliases", kind: "reg" },
    { old: "enum_enumerator.cpp", to: "scopes/04_enumerators", kind: "reg" },
    { old: "lambda_variable.cpp", to: "scopes/05_lambda_captures", kind: "reg" },
    { old: "structured_binding.cpp", to: "scopes/06_structured_bindings", kind: "reg" },
    { old: "function_pointer.cpp", to: "scopes/07_function_pointers", kind: "reg" },
    { old: "default_argument.cpp", to: "scopes/08_default_arguments", kind: "reg" },
    // positions
    { old: "unicode_columns.cpp", to: "positions/01_unicode_columns", kind: "reg" },
    { old: "negative_controls.cpp", to: "positions/02_no_symbol", kind: "reg" },
];

/// Differences between an old block and its new one that are deliberate:
/// `case marker` → why.
const EXPLAINED: Record<string, string> = {};

const SECTIONS: Record<string, string> = {
    definition: "definition",
    declaration: "declaration",
    typeDefinition: "type_definition",
    implementation: "implementation",
    references: "references",
    callHierarchy: "call_hierarchy",
    incomingCalls: "incoming_calls",
    outgoingCalls: "outgoing_calls",
    typeHierarchy: "type_hierarchy",
    supertypes: "supertypes",
    subtypes: "subtypes",
};

/// The requests a marker is asked, in the old suite and the new.
const REQUESTS = Object.keys(SECTIONS).length;

function item(files: OldFiles, query: string, fields: Record<string, string>): Table {
    const at = files.place(
        query,
        fields["file"] ?? "",
        fields["selection"] ?? fields["range"] ?? "",
    );
    return { name: fields["name"] ?? "", kind: fields["kind"] ?? "", at };
}

function sites(files: OldFiles, query: string, file: string, ranges: string): string[] {
    const list = JSON.parse(ranges) as string[];
    return list.map((range) => files.place(query, file, range));
}

/// The old block's entries as the new block shows them, by key.
function expected(files: OldFiles, query: string, block: OldBlock): Map<string, string[]> {
    const out = new Map<string, string[]>();
    const push = (key: string, value: string): void => {
        out.set(key, [...(out.get(key) ?? []), value]);
    };
    const roots = block.entries
        .filter((entry) => entry.section === "callHierarchy")
        .map((entry) => entry.fields["file"] ?? "");
    for (const { section, fields } of block.entries) {
        const key = SECTIONS[section];
        if (key === undefined) {
            throw new Error(`unknown section ${section}`);
        }
        if (fields["range"] !== undefined && fields["name"] === undefined) {
            push(key, files.place(query, fields["file"] ?? "", fields["range"]));
        } else if (section === "incomingCalls") {
            const at = item(files, query, fields);
            const from = sites(files, query, fields["file"] ?? "", fields["fromRanges"] ?? "[]");
            push(key, JSON.stringify({ ...at, sites: from }));
        } else if (section === "outgoingCalls") {
            const at = item(files, query, fields);
            // Outgoing sites lie in the root's file: one of the prepared items'.
            const options = [...new Set(roots)].map((root) =>
                JSON.stringify({
                    ...at,
                    sites: sites(files, query, root, fields["fromRanges"] ?? "[]"),
                }),
            );
            push(key, options.join(" | "));
        } else {
            if (fields["detail"] !== undefined) {
                throw new Error("an old item with a detail");
            }
            push(key, JSON.stringify(item(files, query, fields)));
        }
    }
    return out;
}

function shown(value: unknown): string[] {
    if (!Array.isArray(value)) {
        return [];
    }
    return value.map((entry: unknown) => {
        if (typeof entry === "string") {
            return stripOffset(entry);
        }
        const table = entry as Record<string, unknown>;
        const at = typeof table["at"] === "string" ? stripOffset(table["at"]) : table["at"];
        const rest = Array.isArray(table["sites"])
            ? { sites: (table["sites"] as string[]).map(stripOffset) }
            : {};
        return JSON.stringify({ ...table, at, ...rest });
    });
}

function main(): void {
    const c = corpus("navigation");
    const fixtures = new Map<string, SnapFixture>(c.fixtures.map((f) => [f.rel, f]));
    let oldAsked = 0;
    let asked = 0;
    const notAsked = new Map<string, number>();
    let unexplained = 0;
    const dropped: string[] = [];
    const used = new Set<string>();
    for (const step of PLAN) {
        const fixture = fixtures.get(step.old);
        if (fixture === undefined) {
            throw new Error(`no fixture ${step.old}`);
        }
        if (!fixture.active) {
            continue;
        }
        const blocks = oldBlocks(c, fixture);
        oldAsked += blocks.length * REQUESTS;
        if (step.kind === "drop") {
            dropped.push(
                `${step.old} (${String(blocks.length)} markers): ${step.to}, ${step.why ?? ""}`,
            );
            continue;
        }
        const files = new OldFiles(c, fixture);
        const answers = newBlocks(`navigation/${step.to}`).get("answers");
        for (const block of blocks) {
            const query = files.unitPath(block.file);
            const marker = step.rename?.[block.name] ?? block.name;
            const key = `${step.to} ${marker}`;
            const table = answers?.get(marker);
            const problems: string[] = [];
            if (table === undefined) {
                problems.push("no block");
            } else {
                const want = expected(files, query, block);
                const keys = new Set(Object.keys(table));
                keys.delete("wrong");
                asked += keys.size;
                for (const [k, entries] of want) {
                    if (!keys.has(k)) {
                        notAsked.set(k, (notAsked.get(k) ?? 0) + entries.length);
                    }
                }
                for (const k of keys) {
                    const have = shown(table[k]);
                    const old = want.get(k) ?? [];
                    // An outgoing call matches any of its root files.
                    const lost = missing(old, have).filter(
                        (entry) => !entry.split(" | ").some((option) => have.includes(option)),
                    );
                    const gained = missing(
                        have,
                        old.flatMap((entry) => entry.split(" | ")),
                    );
                    for (const entry of lost) {
                        problems.push(`${k} lost ${entry}`);
                    }
                    for (const entry of gained) {
                        problems.push(`${k} gained ${entry}`);
                    }
                }
            }
            if (problems.length === 0) {
                continue;
            }
            const why = EXPLAINED[key];
            if (why !== undefined) {
                used.add(key);
                continue;
            }
            unexplained += 1;
            console.log(`${step.old} → ${key}\n  ${problems.join("\n  ")}`);
        }
    }
    for (const key of Object.keys(EXPLAINED)) {
        if (!used.has(key)) {
            console.log(`explained but no difference: ${key}`);
            unexplained += 1;
        }
    }
    console.log(`\ndropped:\n  ${dropped.join("\n  ")}`);
    console.log(
        `old: ${String(oldAsked / REQUESTS)} markers × ${String(REQUESTS)} requests = ${String(oldAsked)}`,
    );
    console.log(`asked again at the old markers: ${String(asked)} requests`);
    const counts = [...notAsked].map(([key, count]) => `${key} ${String(count)}`);
    console.log(`old entries of requests no longer asked (d5): ${counts.join(", ")}`);
    console.log(`unexplained differences: ${String(unexplained)}`);
    process.exitCode = unexplained === 0 ? 0 : 1;
}

main();
