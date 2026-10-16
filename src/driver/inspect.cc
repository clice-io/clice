module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :command.command;
import :command.toolchain;
import :compile.compilation;
import :compile.tidy;
import :config.config;
import :driver.driver;
import :feature.feature;
import :index.query;
import :index.shard;
import :index.tu_index;
import :project.command_resolver;
import :project.configuration;
import :project.load;
import :project.project;
import :semantic.symbol;
import :vfs.file_system;
import :vfs.path;

namespace clice::driver {

namespace {

/// What every feature takes.
struct InspectOptions {
    DecoInput(meta_var = "<PATH>", help = "The source file", required = false)
    <std::vector<std::string>> path;

    DecoKV(help = R"(JSON file of the markers in the inspected source file, at byte )"
                  R"(offsets: {"points": {"name": 12}, "ranges": {"name": )"
                  R"({"begin": 3, "end": 7}}}; results are keyed by these names)",
           required = false)
    <std::string> markers;

    DecoKV(help =
               "Compile flags for the file as a JSON string array; "
               "replaces the compile_commands.json lookup",
           required = false)
    <std::string> flags;

    DecoKV(help =
               "Configuration overlay as a JSON object of clice.toml "
               "sections, over the workspace's configuration",
           required = false)
    <std::string> config_overlay;

    ConfigurationOption configuration;
};

struct FeatureOptions {
    kota::deco::decl::HelpOption help;
    InspectOptions inspect;
    LogLevelOption log;
};

/// The options of a feature that checks its own results.
struct CheckedFeatureOptions {
    kota::deco::decl::HelpOption help;
    InspectOptions inspect;

    DecoFlag(names = {"--check"},
             help =
                 "Check the results instead of printing them: report where they differ from "
                 "the same results computed another way",
             required = false)
    check;

    LogLevelOption log;
};

struct SchemaOptions {
    kota::deco::decl::HelpOption help;
    LogLevelOption log;
};

/// JSON layout of the inspect output. Field names stay snake_case (the
/// project's native spelling) and enums serialize as their C++ value
/// names; the TS side owns any mapping to LSP vocabulary. Text that is not
/// UTF-8 is written as U+FFFD, as the server writes it to the client.
struct InspectJsonConfig {
    constexpr static auto enum_repr = kota::codec::enum_repr::String;
    constexpr static auto invalid_utf8 = kota::codec::invalid_utf8::Replace;
};

struct FileEntry {
    /// The whole-document feature payload, absent when compilation failed
    /// or the feature is marker-driven.
    std::optional<kota::codec::RawValue> result;

    /// Marker-driven payloads, keyed by the names --markers gives: position
    /// features run once per point, range features once per range. A null
    /// value records a position with no result (e.g. hover on whitespace).
    std::optional<std::map<std::string, kota::codec::RawValue>> markers;

    std::optional<std::string> error;
    std::optional<std::vector<std::string>> diagnostics;
};

/// The inspected file's markers, as --markers gives them.
struct FileMarkers {
    KOTATSU_ANNOTATE(defaulted = true)
    <std::map<std::string, std::uint32_t>> points;

    KOTATSU_ANNOTATE(defaulted = true)
    <std::map<std::string, LocalSourceRange>> ranges;
};

/// `files` holds the inspected file's entry under its file name.
struct InspectOutput {
    std::string feature;
    std::map<std::string, FileEntry> files;
};

template <typename T>
std::optional<kota::codec::RawValue> to_raw_json(const T& value) {
    auto json = kota::codec::json::to_string<InspectJsonConfig>(value);
    if(!json) {
        LOG_ERROR("serialization failed: {}", json.error().message);
        return std::nullopt;
    }
    return kota::codec::RawValue{std::move(*json)};
}

/// Marker payload for hover: the feature-layer reply before the edge —
/// markdown produced by the same rendering code the server uses, with the
/// symbol range still in byte offsets.
struct HoverResult {
    std::optional<LocalSourceRange> range;
    feature::protocol::MarkupKind kind;
    std::string contents;
};

/// Strict decode for --config and --markers: an unknown key is a typo,
/// not something to silently ignore.
struct StrictJson {
    constexpr static bool deny_unknown_fields = true;
};

std::optional<kota::codec::RawValue> run_folding_ranges(CompilationUnit& unit,
                                                        [[maybe_unused]] const Config& config) {
    return to_raw_json(feature::folding_ranges(unit));
}

std::optional<kota::codec::RawValue> run_semantic_tokens(CompilationUnit& unit,
                                                         [[maybe_unused]] const Config& config) {
    return to_raw_json(feature::semantic_tokens(unit));
}

std::optional<kota::codec::RawValue> run_document_symbols(CompilationUnit& unit,
                                                          const Config& config) {
    return to_raw_json(feature::document_symbols(unit, config.document_symbols));
}

/// One way the outline the index projects differs from the AST's, at the
/// name of the entry it concerns (1-based line and column).
struct OutlineDifference {
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    /// only_ast, only_index, name, kind, range, parent, detail or
    /// deprecated.
    std::string field;
    std::string ast;
    std::string index;
};

struct OutlineCheck {
    std::uint32_t ast = 0;
    std::uint32_t index = 0;
    std::vector<OutlineDifference> differences;
};

/// The file's outline as the AST gives it beside the one the index projects
/// from the unit's own TU index, as the server serves a file before its AST
/// is current: an entry of one matches the other's entry of the same name
/// range, the same name first. The index knows no signatures, so a detail
/// differs only where the index gives one.
std::optional<kota::codec::RawValue> run_document_symbol_check(CompilationUnit& unit,
                                                               const Config& config) {
    auto ast = feature::document_symbols(unit, config.document_symbols);
    auto bytes = index::build_tu_index(unit);
    auto tu = index::TUIndex::from_bytes(bytes);
    auto projected = feature::index_document_symbols(
        unit.focus_file().content(),
        unit.lang_options(),
        feature::extract_index_entries(tu.shard_of(tu.path_count() - 1)),
        [&](index::SymbolHash hash) -> std::optional<index::SymbolRef> {
            auto identity = tu.find_symbol(hash);
            if(!identity) {
                return std::nullopt;
            }
            return index::SymbolRef::from(hash, *identity);
        },
        config.document_symbols);

    struct Flat {
        const feature::DocumentSymbol* symbol;
        std::int32_t parent;
    };

    auto flatten = [](this auto& self,
                      llvm::ArrayRef<feature::DocumentSymbol> symbols,
                      std::int32_t parent,
                      std::vector<Flat>& out) -> void {
        for(const auto& symbol: symbols) {
            auto at = static_cast<std::int32_t>(out.size());
            out.push_back({&symbol, parent});
            self(symbol.children, at, out);
        }
    };
    std::vector<Flat> left;
    std::vector<Flat> right;
    flatten(ast, -1, left);
    flatten(projected, -1, right);

    std::multimap<std::pair<std::uint32_t, std::uint32_t>, std::int32_t> by_name_range;
    for(auto [at, flat]: llvm::enumerate(right)) {
        auto selection = flat.symbol->selection_range;
        by_name_range.emplace(std::pair(selection.begin, selection.end),
                              static_cast<std::int32_t>(at));
    }
    std::vector<std::int32_t> partner(left.size(), -1);
    std::vector<bool> taken(right.size(), false);
    for(auto [at, flat]: llvm::enumerate(left)) {
        auto selection = flat.symbol->selection_range;
        auto [first, last] = by_name_range.equal_range(std::pair(selection.begin, selection.end));
        std::int32_t chosen = -1;
        for(auto it = first; it != last; ++it) {
            if(taken[it->second]) {
                continue;
            }
            if(chosen == -1) {
                chosen = it->second;
            }
            if(right[it->second].symbol->name == flat.symbol->name) {
                chosen = it->second;
                break;
            }
        }
        if(chosen != -1) {
            partner[at] = chosen;
            taken[chosen] = true;
        }
    }

    const auto& map = unit.focus_file().positions();
    auto spell_range = [&](LocalSourceRange range) {
        auto converted = map.range(range, PositionEncoding::UTF8);
        if(!converted) {
            return std::string("?");
        }
        return std::format("{}:{}-{}:{}",
                           converted->start.line + 1,
                           converted->start.character + 1,
                           converted->end.line + 1,
                           converted->end.character + 1);
    };
    auto kind_name = [](SymbolKind kind) {
        return std::string(kota::meta::enum_name(feature::to_protocol_symbol_kind(kind), "?"));
    };
    auto parent_name = [](const std::vector<Flat>& flats, std::int32_t parent) {
        return parent == -1 ? std::string("(top level)") : flats[parent].symbol->name;
    };

    OutlineCheck check{
        .ast = static_cast<std::uint32_t>(left.size()),
        .index = static_cast<std::uint32_t>(right.size()),
    };
    auto report = [&](const feature::DocumentSymbol& symbol,
                      llvm::StringRef field,
                      std::string ast_value,
                      std::string index_value) {
        auto position = map.position(symbol.selection_range.begin, PositionEncoding::UTF8);
        check.differences.push_back({
            .line = position ? position->line + 1 : 0,
            .column = position ? position->character + 1 : 0,
            .field = field.str(),
            .ast = std::move(ast_value),
            .index = std::move(index_value),
        });
    };
    for(auto [at, flat]: llvm::enumerate(left)) {
        const auto& lhs = *flat.symbol;
        if(partner[at] == -1) {
            report(lhs, "only_ast", std::format("{} {}", kind_name(lhs.kind), lhs.name), "");
            continue;
        }
        const auto& other = right[partner[at]];
        const auto& rhs = *other.symbol;
        if(lhs.name != rhs.name) {
            report(lhs, "name", lhs.name, rhs.name);
        }
        if(kind_name(lhs.kind) != kind_name(rhs.kind)) {
            report(lhs, "kind", kind_name(lhs.kind), kind_name(rhs.kind));
        }
        if(lhs.range != rhs.range) {
            report(lhs, "range", spell_range(lhs.range), spell_range(rhs.range));
        }
        bool same_parent =
            flat.parent == -1 ? other.parent == -1 : partner[flat.parent] == other.parent;
        if(!same_parent) {
            report(lhs, "parent", parent_name(left, flat.parent), parent_name(right, other.parent));
        }
        if(!rhs.detail.empty() && lhs.detail != rhs.detail) {
            report(lhs, "detail", lhs.detail, rhs.detail);
        }
        if(lhs.deprecated != rhs.deprecated) {
            report(lhs,
                   "deprecated",
                   lhs.deprecated ? "true" : "false",
                   rhs.deprecated ? "true" : "false");
        }
    }
    for(auto [at, flat]: llvm::enumerate(right)) {
        if(!taken[at]) {
            const auto& rhs = *flat.symbol;
            report(rhs, "only_index", "", std::format("{} {}", kind_name(rhs.kind), rhs.name));
        }
    }
    std::ranges::stable_sort(check.differences, {}, [](const OutlineDifference& difference) {
        return std::pair(difference.line, difference.column);
    });
    return to_raw_json(check);
}

std::optional<kota::codec::RawValue> run_document_links(CompilationUnit& unit,
                                                        [[maybe_unused]] const Config& config) {
    return to_raw_json(feature::document_links(unit));
}

struct RawLocation {
    std::string path;
    kota::ipc::protocol::Range range;
};

struct RawInlayHintPart {
    std::string value;
    /// Where the piece links, for one naming a symbol the index places or
    /// a target in the main file.
    std::optional<RawLocation> location;
};

struct RawInlayHint {
    std::uint32_t offset = 0;
    feature::HintCategory kind;
    std::vector<RawInlayHintPart> label;
    bool padding_left = false;
    bool padding_right = false;
};

/// Range payload for inlay hints: each label piece naming a symbol placed
/// as the server places it, from the unit's own TU index standing in for
/// the server's — a declaration, else the definition, the main file's shard
/// first as an open buffer's is.
std::optional<kota::codec::RawValue> run_inlay_hints(CompilationUnit& unit,
                                                     LocalSourceRange range,
                                                     const Config& config) {
    auto hints = feature::inlay_hints(unit, range, config.inlay_hints);

    std::string bytes;
    index::TUIndex tu;
    auto place = [&](index::SymbolHash symbol) -> std::optional<RawLocation> {
        if(bytes.empty()) {
            bytes = index::build_tu_index(unit, {.all_files = true});
            tu = index::TUIndex::from_bytes(bytes);
        }
        auto main = tu.path_count() - 1;
        for(auto kind: {RelationKind::Declaration, RelationKind::Definition}) {
            for(std::uint32_t i = 0; i < tu.path_count(); i += 1) {
                auto path_id = i == 0 ? main : i - 1;
                const auto& shard = tu.shard_of(path_id);
                std::optional<LocalSourceRange> found;
                shard.lookup(symbol, kind, [&](const index::Relation& relation) {
                    found = relation.range;
                    return false;
                });
                if(!found) {
                    continue;
                }
                return RawLocation{
                    .path = tu.path(path_id).str(),
                    .range = *shard.positions().range(*found, PositionEncoding::UTF16),
                };
            }
        }
        return std::nullopt;
    };

    std::vector<RawInlayHint> out;
    for(auto& hint: hints) {
        RawInlayHint raw{
            .offset = hint.offset,
            .kind = hint.kind,
            .padding_left = hint.padding_left,
            .padding_right = hint.padding_right,
        };
        for(auto& part: hint.label) {
            raw.label.push_back({.value = std::move(part.value)});
            if(part.symbol) {
                raw.label.back().location = place(part.symbol);
            } else if(part.target) {
                auto position =
                    unit.focus_file().positions().position(*part.target, PositionEncoding::UTF16);
                raw.label.back().location = RawLocation{
                    .path = unit.file_path(unit.focus_file().file()).str(),
                    .range = {*position, *position},
                };
            }
        }
        out.push_back(std::move(raw));
    }
    return to_raw_json(out);
}

std::optional<kota::codec::RawValue> run_selection_range(CompilationUnit& unit,
                                                         std::uint32_t offset,
                                                         [[maybe_unused]] const Config& config) {
    return to_raw_json(feature::selection_ranges(unit, {offset}).front());
}

struct RawCodeAction {
    std::string title;
    std::string kind;
    std::vector<TextReplacement> edits;
    /// The definitions a host-source request would place; the request
    /// itself stays unresolved here, having no host and no index.
    std::optional<std::vector<std::string>> host_definitions;
};

/// The index requests resolved as an empty index would: definitions into
/// the main file are all kept, no header declares any name.
std::optional<kota::codec::RawValue> run_code_action(CompilationUnit& unit,
                                                     LocalSourceRange selection,
                                                     [[maybe_unused]] const Config& config) {
    std::vector<RawCodeAction> out;
    auto path = unit.file_path(unit.focus_file().file());
    for(auto& action: feature::code_actions(unit, selection)) {
        RawCodeAction raw{
            .title = std::move(action.title),
            .kind = std::string(action.kind),
            .edits = std::move(action.edits),
        };
        if(action.index) {
            auto keep = [](std::uint64_t) {
                return false;
            };
            if(auto* request = std::get_if<feature::DefineRequest>(&*action.index)) {
                auto text = feature::assemble_definitions(request->pieces, keep);
                raw.edits = feature::format_edits(
                    path,
                    unit.focus_file().content(),
                    {
                        {request->range, request->before + *text + request->after}
                });
            } else if(auto* host = std::get_if<feature::DefineInHostRequest>(&*action.index)) {
                raw.host_definitions.emplace();
                for(const auto& piece: host->pieces) {
                    raw.host_definitions->push_back(piece.text);
                }
            } else {
                continue;
            }
        }
        out.push_back(std::move(raw));
    }
    return to_raw_json(out);
}

/// nullopt = serialization failure; an empty RawValue serializes as null
/// and records a marker with no hover. The card is markdown, as a client
/// that shows markdown gets it.
std::optional<kota::codec::RawValue> run_hover(CompilationUnit& unit,
                                               std::uint32_t offset,
                                               const Config& config) {
    auto info = feature::hover_info(unit, offset, config.hover, config.documentation);
    if(!info) {
        return kota::codec::RawValue{};
    }
    return to_raw_json(HoverResult{info->symbol_range,
                                   feature::protocol::MarkupKind::Markdown,
                                   info->present().as_markdown()});
}

/// A declaration whose hover card prints a definition that does not
/// declare it again.
struct HoverCheckFinding {
    /// The declaration's qualified name.
    std::string name;

    /// Where it is declared, `file:line`.
    std::string location;

    /// The card is an implicit instantiation's.
    bool instantiation = false;

    /// The definition its card prints.
    std::string definition;

    /// What the definition, compiled where the declaration is, gets wrong:
    /// a diagnostic it draws, or a property it gives otherwise.
    std::vector<std::string> problems;
};

struct HoverCheckReport {
    /// How many cards were checked.
    std::uint32_t checked = 0;

    /// How many twins, or their originals, the compiles with the twins do not
    /// declare where they were put.
    std::uint32_t unmatched = 0;

    std::vector<HoverCheckFinding> findings;
};

constexpr llvm::StringLiteral twin_prefix = "clice_twin_";

/// Where a declaration's name is written: its file's path and offset.
struct Place {
    std::string path;
    std::uint32_t offset = 0;

    friend bool operator<(const Place& left, const Place& right) {
        return std::tie(left.path, left.offset) < std::tie(right.path, right.offset);
    }
};

/// A checked card: the declaration it shows and the twin its definition
/// declares.
struct Twin {
    std::string name;

    /// The declaration in the first compile, and where its name is.
    const clang::NamedDecl* original = nullptr;
    Place place;

    /// An implicit instantiation's card: its template's place, and its
    /// arguments as printed.
    bool instantiation = false;
    std::string arguments;

    /// A class template's or a partial specialization's card: its body
    /// goes in the class without the template head, which goes after it as
    /// `clice_twin_N_head`.
    bool templated_record = false;

    /// The name a specialization's or a templated class's card prints its
    /// head with, which its twin's names instead; empty for a renamed one.
    std::string head_name;

    struct Insertion {
        /// Where the text goes in the first compile's file.
        Place at;
        std::string text;

        /// The text's range in the second compile.
        std::uint32_t begin = 0;
        std::uint32_t end = 0;
    };

    std::vector<Insertion> insertions;

    /// The member types a nested twin's card declares without a body
    /// (`struct Node;`), left out of it: declared again in the twin, they
    /// would hide the class's own.
    std::vector<std::string> member_types;

    /// The bodies of the lambdas the declaration writes, in order: the
    /// card leaves them out, and a lambda's return type is deduced from it.
    std::vector<std::string> lambda_bodies;

    HoverCheckFinding finding;
};

/// The bodies of the outermost lambdas a declaration writes, as written.
struct LambdaBodies : clang::RecursiveASTVisitor<LambdaBodies> {
    const clang::ASTContext& context;
    std::vector<std::string> bodies;

    /// A card prints no function's body.
    bool TraverseDecl(clang::Decl* decl) {
        return llvm::isa_and_present<clang::FunctionDecl>(decl) ||
               clang::RecursiveASTVisitor<LambdaBodies>::TraverseDecl(decl);
    }

    bool TraverseLambdaExpr(clang::LambdaExpr* lambda) {
        auto range = clang::Lexer::makeFileCharRange(
            clang::CharSourceRange::getTokenRange(lambda->getBody()->getSourceRange()),
            context.getSourceManager(),
            context.getLangOpts());
        bodies.push_back(
            clang::Lexer::getSourceText(range, context.getSourceManager(), context.getLangOpts())
                .str());
        return true;
    }
};

bool is_checked(const clang::NamedDecl& decl) {
    if(decl.isImplicit() || decl.isInvalidDecl() || decl.getLocation().isMacroID() ||
       decl.getParentFunctionOrMethod() || !decl.getIdentifier() || !decl.isFirstDecl()) {
        return false;
    }
    if(const auto* var = llvm::dyn_cast<clang::VarDecl>(&decl)) {
        return !llvm::isa<clang::ParmVarDecl,
                          clang::DecompositionDecl,
                          clang::VarTemplateSpecializationDecl>(var);
    }
    if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(&decl)) {
        return !function->isFunctionTemplateSpecialization() &&
               function->getFriendObjectKind() == clang::Decl::FOK_None;
    }
    if(const auto* tag = llvm::dyn_cast<clang::TagDecl>(&decl)) {
        const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(tag);
        if(record && (record->isLambda() || record->isInjectedClassName())) {
            return false;
        }
        if(const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(tag);
           spec && spec->getSpecializationKind() != clang::TSK_ExplicitSpecialization) {
            return false;
        }
        return tag->isThisDeclarationADefinition();
    }
    return llvm::isa<clang::FieldDecl, clang::TypedefNameDecl, clang::ConceptDecl>(decl);
}

struct Collector : clang::RecursiveASTVisitor<Collector> {
    std::vector<const clang::NamedDecl*> decls;
    std::vector<const clang::ClassTemplateSpecializationDecl*> instantiations;

    bool shouldVisitTemplateInstantiations() const {
        return false;
    }

    bool VisitNamedDecl(clang::NamedDecl* decl) {
        if(is_checked(*decl)) {
            decls.push_back(decl);
        }
        if(const auto* class_template = llvm::dyn_cast<clang::ClassTemplateDecl>(decl);
           class_template && class_template->isThisDeclarationADefinition()) {
            for(const auto* spec: class_template->specializations()) {
                if(spec->getSpecializationKind() == clang::TSK_ImplicitInstantiation &&
                   spec->isCompleteDefinition()) {
                    instantiations.push_back(spec);
                }
            }
        }
        return true;
    }
};

/// Gives declarations a name for the card to print and gives them back
/// theirs after.
struct Renaming {
    std::vector<std::pair<clang::NamedDecl*, clang::DeclarationName>> saved;

    void rename(const clang::NamedDecl* decl, clang::DeclarationName name) {
        for(auto* redecl: decl->redecls()) {
            auto* named = llvm::cast<clang::NamedDecl>(redecl);
            saved.emplace_back(named, named->getDeclName());
            named->setDeclName(name);
        }
    }

    void rename_declaration(const clang::NamedDecl& decl, clang::ASTContext& context) {
        // A templated class's or a specialization's twin is a class nested
        // in it, where its name keeps naming it.
        if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(&decl);
           record && (record->getDescribedClassTemplate() ||
                      llvm::isa<clang::ClassTemplateSpecializationDecl>(record))) {
            return;
        }
        auto& identifier = context.Idents.get(twin_name);
        clang::DeclarationName name(&identifier);
        rename(&decl, name);
        if(const auto* described = decl.getDescribedTemplate()) {
            rename(described, name);
        }
        if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(&decl)) {
            if(const auto* described = function->getDescribedFunctionTemplate()) {
                rename(described, name);
            }
        }
        if(const auto* alias = llvm::dyn_cast<clang::TypeAliasDecl>(&decl)) {
            if(const auto* described = alias->getDescribedAliasTemplate()) {
                rename(described, name);
            }
        }
        if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(&decl)) {
            for(const auto* member: record->decls()) {
                if(const auto* injected = llvm::dyn_cast<clang::CXXRecordDecl>(member);
                   injected && injected->isInjectedClassName()) {
                    rename(injected, name);
                }
            }
        }
        // An unscoped enum's enumerators are declared around it: the twin's
        // would clash with the original's.
        // So would C's, of the enums in a struct.
        rename_enumerators(decl, context);
    }

    void rename_enumerators(const clang::Decl& decl, clang::ASTContext& context) {
        if(const auto* enum_decl = llvm::dyn_cast<clang::EnumDecl>(&decl);
           enum_decl && !enum_decl->isScoped()) {
            for(const auto* enumerator: enum_decl->enumerators()) {
                auto& renamed = context.Idents.get((twin_name + "_" + enumerator->getName()).str());
                rename(enumerator, clang::DeclarationName(&renamed));
            }
        }
        if(const auto* record = llvm::dyn_cast<clang::RecordDecl>(&decl);
           record && !context.getLangOpts().CPlusPlus) {
            for(const auto* member: record->decls()) {
                if(llvm::isa<clang::TagDecl>(member)) {
                    rename_enumerators(*member, context);
                }
            }
        }
    }

    ~Renaming() {
        for(auto& [decl, name]: llvm::reverse(saved)) {
            decl->setDeclName(name);
        }
    }

    llvm::StringRef twin_name;
};

/// The offset of the end of the balanced `<...>` starting at `at`.
auto skip_angles(llvm::StringRef text, std::size_t at) -> std::size_t {
    // Brackets in parentheses, and `<=`, `>=`, `->`, are no template's.
    int depth = 0;
    int parens = 0;
    for(; at < text.size(); at += 1) {
        auto c = text[at];
        auto next = at + 1 < text.size() ? text[at + 1] : '\0';
        auto previous = at > 0 ? text[at - 1] : '\0';
        if(c == '(' || c == '[' || c == '{') {
            parens += 1;
        } else if(c == ')' || c == ']' || c == '}') {
            parens -= 1;
        } else if(parens != 0 || next == '=' || previous == '-') {
            continue;
        } else if(c == '<') {
            depth += 1;
        } else if(c == '>') {
            depth -= 1;
            if(depth == 0) {
                return at + 1;
            }
        }
    }
    return text.size();
}

/// Where the class key before `name` starts, past the template head and
/// any requires clause: npos when there is none.
auto class_key(llvm::StringRef text, llvm::StringRef name) -> std::size_t {
    auto at = text.find(name);
    if(at == llvm::StringRef::npos) {
        return at;
    }
    auto head = text.take_front(at);
    std::size_t key = llvm::StringRef::npos;
    for(llvm::StringRef word: {"struct", "class", "union"}) {
        for(auto found = head.rfind(word); found != llvm::StringRef::npos;
            found = head.take_front(found).rfind(word)) {
            bool starts = found == 0 || !clang::isAsciiIdentifierContinue(head[found - 1]);
            bool ends = found + word.size() < head.size() &&
                        !clang::isAsciiIdentifierContinue(head[found + word.size()]);
            if(starts && ends) {
                key = key == llvm::StringRef::npos ? found : std::max(key, found);
                break;
            }
        }
    }
    return key;
}

/// The card's definition as the twin's declaration: a specialization's head
/// names the twin, a class of its own; a templated class's head is left to
/// its own twin.
auto twin_text(llvm::StringRef text, Twin& twin, bool nested) -> std::string {
    std::string prefix;
    if(text.consume_front("export ") && !nested) {
        prefix = "export ";
    }
    std::string rest = text.str();
    // `template <...> struct std::Box<T *> : Base {` as
    // `struct clice_twin_0 : Base {`.
    if(!twin.head_name.empty()) {
        auto key = class_key(text, twin.head_name);
        if(key != llvm::StringRef::npos) {
            auto name = text.find(twin.head_name, key);
            auto start = name;
            while(start > key &&
                  (clang::isAsciiIdentifierContinue(text[start - 1]) || text[start - 1] == ':')) {
                start -= 1;
            }
            // The class key itself, `struct` or `class`.
            if(start == key) {
                start = text.find(' ', key) + 1;
            }
            auto end = name + twin.head_name.size();
            if(end < text.size() && text[end] == '<') {
                end = skip_angles(text, end);
            }
            rest = (text.substr(key, start - key) + twin.name + text.substr(end)).str();
        }
    }
    if(nested || twin.instantiation) {
        const static std::regex forward(
            R"(^\s*(?:(?:template\s*<.*>\s*)?(?:struct|class|union)\s+(\w+)|enum\s+(?:class|struct)\s+(\w+)(?:\s*:[^;{]*)?|enum\s+(\w+)\s*:[^;{]*)\s*;\s*$)");
        std::string kept;
        int depth = 0;
        for(auto line: llvm::split(rest, '\n')) {
            std::smatch match;
            std::string owned = line.str();
            if(depth == 1 && std::regex_match(owned, match, forward)) {
                twin.member_types.push_back(match[1].matched   ? match[1].str()
                                            : match[2].matched ? match[2].str()
                                                               : match[3].str());
            } else {
                kept += owned + "\n";
            }
            depth += llvm::count(line, '{') - llvm::count(line, '}');
        }
        rest = kept;
    }
    std::string result = prefix + rest;
    std::size_t lambda = 0;
    for(auto at = result.find("{ ... }"); at != std::string::npos;
        at = result.find("{ ... }", at)) {
        auto body = lambda < twin.lambda_bodies.size() ? twin.lambda_bodies[lambda] : "{}";
        lambda += 1;
        result.replace(at, 7, body);
        at += body.size();
    }
    if(!llvm::StringRef(result).rtrim().ends_with(";")) {
        result += ';';
    }
    return result;
}

/// The offset in its file where a declaration ends: after its body's `}`,
/// or after the `;` ending the declaration it is part of.
auto declaration_end(CompilationUnit& unit, const clang::NamedDecl& decl)
    -> std::optional<std::uint32_t> {
    auto& sources = unit.context().getSourceManager();
    const auto* function = llvm::dyn_cast<clang::FunctionDecl>(&decl);
    clang::SourceLocation end = decl.getEndLoc();
    if(const auto* described = decl.getDescribedTemplate()) {
        end = described->getEndLoc();
    }
    end = sources.getExpansionRange(end).getEnd();
    if(end.isInvalid()) {
        return std::nullopt;
    }
    auto [fid, offset] = sources.getDecomposedLoc(end);
    auto content = sources.getBufferData(fid);
    offset += clang::Lexer::MeasureTokenLength(end, sources, unit.lang_options());
    if(function && function->doesThisDeclarationHaveABody() && function->getBody() &&
       content.substr(0, offset).ends_with("}")) {
        return offset;
    }

    clang::Lexer lexer(sources.getLocForStartOfFile(fid),
                       unit.lang_options(),
                       content.begin(),
                       content.begin() + offset,
                       content.end());
    int depth = 0;
    clang::Token token;
    while(!lexer.LexFromRawLexer(token)) {
        switch(token.getKind()) {
            case clang::tok::l_paren:
            case clang::tok::l_square:
            case clang::tok::l_brace: depth += 1; break;
            case clang::tok::r_paren:
            case clang::tok::r_square:
            case clang::tok::r_brace:
                if(depth == 0) {
                    return std::nullopt;
                }
                depth -= 1;
                break;
            case clang::tok::semi:
                if(depth == 0) {
                    return sources.getFileOffset(token.getLocation()) + 1;
                }
                break;
            default: break;
        }
    }
    return std::nullopt;
}

/// The namespaces around a declaration, opened and closed again around its
/// twin; nullopt when a class is around it.
auto reopened(const clang::Decl& decl) -> std::optional<std::pair<std::string, std::string>> {
    std::string open;
    std::string close;
    for(const auto* context = decl.getDeclContext(); !context->isTranslationUnit();
        context = context->getParent()) {
        const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(context);
        if(llvm::isa<clang::LinkageSpecDecl>(context)) {
            continue;
        }
        if(!ns) {
            return std::nullopt;
        }
        open.insert(0,
                    std::format("{}namespace {} {{ ",
                                ns->isInline() ? "inline " : "",
                                ns->isAnonymousNamespace() ? "" : ns->getName().str()));
        close += "} ";
    }
    return std::pair(open, close);
}

auto attribute_names(const clang::Decl& decl, clang::ASTContext& context)
    -> std::vector<std::string> {
    std::vector<std::string> names;
    for(const auto* attr: decl.attrs()) {
        if(attr->isImplicit() || attr->isInherited()) {
            continue;
        }
        std::string name = attr->getSpelling();
        if(const auto* aligned = llvm::dyn_cast<clang::AlignedAttr>(attr);
           aligned && !aligned->isAlignmentDependent()) {
            name += std::format("({})", aligned->getAlignment(context) / context.getCharWidth());
        }
        names.push_back(std::move(name));
    }
    llvm::sort(names);
    return names;
}

/// Compares a declaration of the second compile with its twin.
struct Comparison {
    clang::ASTContext& context;
    std::vector<std::string>& problems;

    /// An implicit instantiation's card leaves out the bases the library
    /// keeps to itself.
    bool instantiation = false;

    /// A templated class's twin is a class of its own; its head is checked
    /// apart.
    bool templated_record = false;

    /// The twin's own types, by the original's they stand for.
    std::map<const clang::Decl*, const clang::Decl*> matched;

    auto spell(clang::QualType type) -> std::string {
        return type.getAsString(context.getPrintingPolicy());
    }

    void add(llvm::StringRef where, std::string problem) {
        problems.push_back(where.empty() ? problem : std::format("{}: {}", where, problem));
    }

    bool same_decl(const clang::Decl* original, const clang::Decl* twin) {
        if(!original || !twin) {
            return original == twin;
        }
        original = original->getCanonicalDecl();
        twin = twin->getCanonicalDecl();
        return original == twin || (matched.contains(twin) && matched.at(twin) == original);
    }

    /// Whether two unnamed types declare the same members; matched if so.
    bool same_shape(const clang::TagDecl& original, const clang::TagDecl& twin) {
        if(original.getDeclName() || twin.getDeclName() || original.getKind() != twin.getKind()) {
            return false;
        }
        const auto* left = original.getDefinition();
        const auto* right = twin.getDefinition();
        if(!left || !right) {
            return false;
        }
        matched[right->getCanonicalDecl()] = left->getCanonicalDecl();
        bool same = true;
        if(const auto* record = llvm::dyn_cast<clang::RecordDecl>(left)) {
            auto fields = llvm::cast<clang::RecordDecl>(right)->fields();
            auto it = fields.begin();
            for(const auto* field: record->fields()) {
                // The twins of its fields are inserted in it too.
                if(field->getName().starts_with(twin_prefix)) {
                    continue;
                }
                if(it == fields.end() || field->getDeclName() != it->getDeclName() ||
                   !same_type(field->getType(), it->getType()) ||
                   field->isBitField() != it->isBitField()) {
                    same = false;
                    break;
                }
                ++it;
            }
            same = same && it == fields.end();
        } else {
            same = std::ranges::distance(llvm::cast<clang::EnumDecl>(left)->enumerators()) ==
                   std::ranges::distance(llvm::cast<clang::EnumDecl>(right)->enumerators());
        }
        if(!same) {
            matched.erase(right->getCanonicalDecl());
        }
        return same;
    }

    auto printed(const clang::Expr* expr) -> std::string {
        std::string text;
        llvm::raw_string_ostream os(text);
        expr->printPretty(os, nullptr, context.getPrintingPolicy());
        return text;
    }

    bool same_expr(const clang::Expr* original, const clang::Expr* twin) {
        if(!original || !twin) {
            return original == twin;
        }
        llvm::FoldingSetNodeID left;
        llvm::FoldingSetNodeID right;
        original->Profile(left, context, /*Canonical=*/true);
        twin->Profile(right, context, /*Canonical=*/true);
        if(left == right) {
            return true;
        }
        // Written the same, as a twin nested in its class names `this` and
        // the members of its own class.
        if(printed(original) == printed(twin)) {
            return true;
        }
        // A lambda by its parameters: the card leaves out the body, which
        // its return type may be deduced from.
        if(const auto* lambda = llvm::dyn_cast<clang::LambdaExpr>(original->IgnoreImplicit())) {
            const auto* other = llvm::dyn_cast<clang::LambdaExpr>(twin->IgnoreImplicit());
            if(!other || lambda->getCallOperator()->getNumParams() !=
                             other->getCallOperator()->getNumParams()) {
                return false;
            }
            return llvm::all_of(llvm::zip(lambda->getCallOperator()->parameters(),
                                          other->getCallOperator()->parameters()),
                                [&](auto pair) {
                                    return same_type(std::get<0>(pair)->getType(),
                                                     std::get<1>(pair)->getType());
                                });
        }
        if(original->isValueDependent() || twin->isValueDependent()) {
            return false;
        }
        clang::Expr::EvalResult first;
        clang::Expr::EvalResult second;
        return original->EvaluateAsRValue(first, context) &&
               twin->EvaluateAsRValue(second, context) &&
               first.Val.getAsString(context, original->getType()) ==
                   second.Val.getAsString(context, twin->getType());
    }

    bool same_args(llvm::ArrayRef<clang::TemplateArgument> original,
                   llvm::ArrayRef<clang::TemplateArgument> twin) {
        if(original.size() != twin.size()) {
            return false;
        }
        for(auto [left, right]: llvm::zip(original, twin)) {
            if(left.getKind() != right.getKind()) {
                return false;
            }
            if(left.getKind() == clang::TemplateArgument::Type) {
                if(!same_type(left.getAsType(), right.getAsType())) {
                    return false;
                }
            } else if(left.getKind() == clang::TemplateArgument::Expression) {
                if(!same_expr(left.getAsExpr(), right.getAsExpr())) {
                    return false;
                }
            } else if(left.getKind() == clang::TemplateArgument::Pack) {
                if(!same_args(left.pack_elements(), right.pack_elements())) {
                    return false;
                }
            } else if(!left.structurallyEquals(right)) {
                return false;
            }
        }
        return true;
    }

    bool same_type(clang::QualType original, clang::QualType twin) {
        if(original.isNull() || twin.isNull()) {
            return original.isNull() == twin.isNull();
        }
        original = original.getCanonicalType();
        twin = twin.getCanonicalType();
        if(context.hasSameType(original, twin)) {
            return true;
        }
        if(original.getQualifiers() != twin.getQualifiers()) {
            return false;
        }
        const auto* left = original.getTypePtr();
        const auto* right = twin.getTypePtr();
        if(left->getTypeClass() != right->getTypeClass()) {
            return false;
        }
        if(const auto* tag = llvm::dyn_cast<clang::TagType>(left)) {
            const auto* other = llvm::cast<clang::TagType>(right)->getDecl();
            // C declares a tag a parameter list names first in that list.
            if(!context.getLangOpts().CPlusPlus && tag->getDecl()->getDeclName() &&
               tag->getDecl()->getDeclName() == other->getDeclName()) {
                return true;
            }
            if(same_decl(tag->getDecl(), other) || same_shape(*tag->getDecl(), *other)) {
                return true;
            }
            const auto* spec =
                llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(tag->getDecl());
            const auto* other_spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(other);
            return spec && other_spec &&
                   spec->getSpecializedTemplate() == other_spec->getSpecializedTemplate() &&
                   same_args(spec->getTemplateArgs().asArray(),
                             other_spec->getTemplateArgs().asArray());
        }
        if(original->isDependentType() && twin->isDependentType() &&
           spell(original) == spell(twin)) {
            return true;
        }
        if(left->isPointerType() || left->isReferenceType()) {
            return same_type(left->getPointeeType(), right->getPointeeType());
        }
        if(const auto* member = llvm::dyn_cast<clang::MemberPointerType>(left)) {
            const auto* other = llvm::cast<clang::MemberPointerType>(right);
            return same_type(member->getPointeeType(), other->getPointeeType()) &&
                   same_decl(member->getMostRecentCXXRecordDecl(),
                             other->getMostRecentCXXRecordDecl());
        }
        if(const auto* array = llvm::dyn_cast<clang::ConstantArrayType>(left)) {
            const auto* other = llvm::cast<clang::ConstantArrayType>(right);
            return array->getSize() == other->getSize() &&
                   same_type(array->getElementType(), other->getElementType());
        }
        if(const auto* array = llvm::dyn_cast<clang::ArrayType>(left)) {
            return same_type(array->getElementType(),
                             llvm::cast<clang::ArrayType>(right)->getElementType());
        }
        if(const auto* function = llvm::dyn_cast<clang::FunctionProtoType>(left)) {
            const auto* other = llvm::cast<clang::FunctionProtoType>(right);
            if(function->getNumParams() != other->getNumParams() ||
               function->isVariadic() != other->isVariadic() ||
               function->getMethodQuals() != other->getMethodQuals() ||
               function->getRefQualifier() != other->getRefQualifier() ||
               function->getExceptionSpecType() != other->getExceptionSpecType() ||
               !same_expr(function->getNoexceptExpr(), other->getNoexceptExpr()) ||
               !same_type(function->getReturnType(), other->getReturnType())) {
                return false;
            }
            for(auto [param, other_param]:
                llvm::zip(function->param_types(), other->param_types())) {
                if(!same_type(param, other_param)) {
                    return false;
                }
            }
            return true;
        }
        if(const auto* id = llvm::dyn_cast<clang::TemplateSpecializationType>(left)) {
            const auto* other = llvm::cast<clang::TemplateSpecializationType>(right);
            return same_decl(id->getTemplateName().getAsTemplateDecl(),
                             other->getTemplateName().getAsTemplateDecl()) &&
                   same_args(id->template_arguments(), other->template_arguments());
        }
        return false;
    }

    void same_type(llvm::StringRef where,
                   llvm::StringRef what,
                   clang::QualType original,
                   clang::QualType twin) {
        if(!same_type(original, twin)) {
            add(where, std::format("{} `{}`, twin `{}`", what, spell(original), spell(twin)));
        }
    }

    void flag(llvm::StringRef where, llvm::StringRef what, bool original, bool twin) {
        if(original != twin) {
            add(where, std::format("{} {}", what, original ? "dropped" : "added"));
        }
    }

    void attributes(llvm::StringRef where, const clang::Decl& original, const clang::Decl& twin) {
        auto left = attribute_names(original, context);
        auto right = attribute_names(twin, context);
        // A renamed override overrides nothing, and clang drops its `override`.
        if(const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(&original);
           method &&
           (method->size_overridden_methods() != 0 || method->hasAttr<clang::OverrideAttr>())) {
            std::erase_if(left, [](const std::string& name) {
                return name == "override" || name == "final";
            });
            std::erase_if(right, [](const std::string& name) {
                return name == "override" || name == "final";
            });
        }
        std::vector<std::string> dropped;
        std::vector<std::string> added;
        std::ranges::set_difference(left, right, std::back_inserter(dropped));
        std::ranges::set_difference(right, left, std::back_inserter(added));
        for(auto& name: dropped) {
            add(where, std::format("attribute {} dropped", name));
        }
        for(auto& name: added) {
            add(where, std::format("attribute {} added", name));
        }
    }

    void template_params(llvm::StringRef where,
                         const clang::TemplateParameterList* original,
                         const clang::TemplateParameterList* twin) {
        if(!original || !twin) {
            if(original != twin) {
                add(where, "template parameters differ");
            }
            return;
        }
        if(original->size() != twin->size()) {
            add(where, "template parameter count differs");
            return;
        }
        for(auto [left, right]: llvm::zip(*original, *twin)) {
            if(left->getKind() != right->getKind() ||
               left->isTemplateParameterPack() != right->isTemplateParameterPack()) {
                add(where, std::format("template parameter {} differs", left->getName()));
                continue;
            }
            if(const auto* value = llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(left)) {
                same_type(where,
                          "template parameter type",
                          value->getType(),
                          llvm::cast<clang::NonTypeTemplateParmDecl>(right)->getType());
            }
            if(const auto* type = llvm::dyn_cast<clang::TemplateTypeParmDecl>(left)) {
                flag(where,
                     "template parameter default",
                     type->hasDefaultArgument(),
                     llvm::cast<clang::TemplateTypeParmDecl>(right)->hasDefaultArgument());
                flag(where,
                     "template parameter constraint",
                     type->hasTypeConstraint(),
                     llvm::cast<clang::TemplateTypeParmDecl>(right)->hasTypeConstraint());
            }
        }
        if(!same_expr(original->getRequiresClause(), twin->getRequiresClause())) {
            add(where, "requires clause differs");
        }
    }

    void variable(llvm::StringRef where,
                  const clang::VarDecl& original,
                  const clang::VarDecl& twin) {
        same_type(where, "type", original.getType(), twin.getType());
        if(original.getStorageClass() != twin.getStorageClass()) {
            add(where, "storage class differs");
        }
        flag(where, "thread_local", original.getTSCSpec() != 0, twin.getTSCSpec() != 0);
        flag(where, "constexpr", original.isConstexpr(), twin.isConstexpr());
        flag(where, "inline", original.isInlineSpecified(), twin.isInlineSpecified());
        // An instantiation's card shows the initializer of the member it is
        // instantiated from, which clang instantiates where it is used; one
        // that depends on the template's arguments has nothing to compare.
        const auto* init = original.getInit();
        if(const auto* pattern = original.getInstantiatedFromStaticDataMember();
           !init && pattern && pattern->getInit()) {
            if(pattern->getInit()->isValueDependent()) {
                return;
            }
            init = pattern->getInit();
        }
        initializer(where, init, twin.getInit());
    }

    void initializer(llvm::StringRef where, const clang::Expr* original, const clang::Expr* twin) {
        if(!original || !twin) {
            flag(where, "initializer", original, twin);
        } else if(!same_expr(original, twin)) {
            add(where, "initializer differs");
        }
    }

    void field(llvm::StringRef where,
               const clang::FieldDecl& original,
               const clang::FieldDecl& twin) {
        same_type(where, "type", original.getType(), twin.getType());
        flag(where, "mutable", original.isMutable(), twin.isMutable());
        if(original.isBitField() != twin.isBitField() ||
           (original.isBitField() && !original.getBitWidth()->isValueDependent() &&
            original.getBitWidthValue() != twin.getBitWidthValue())) {
            add(where, "bit width differs");
        }
        // As for a static data member, of the field it is instantiated from.
        const auto* init = original.getInClassInitializer();
        const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(original.getParent());
        if(const auto* pattern = record ? record->getTemplateInstantiationPattern() : nullptr;
           !init && pattern) {
            const auto* from = *std::next(pattern->field_begin(), original.getFieldIndex());
            if(const auto* written = from->getInClassInitializer()) {
                if(written->isValueDependent()) {
                    return;
                }
                init = written;
            }
        }
        initializer(where, init, twin.getInClassInitializer());
    }

    void function(llvm::StringRef where,
                  const clang::FunctionDecl& original,
                  const clang::FunctionDecl& twin) {
        // A card prints the type a placeholder deduces in its place.
        if(!same_type(original.getDeclaredReturnType(), twin.getDeclaredReturnType()) &&
           !(original.getDeclaredReturnType()->getContainedDeducedType() &&
             same_type(original.getReturnType(), twin.getDeclaredReturnType()))) {
            same_type(where,
                      "return type",
                      original.getDeclaredReturnType(),
                      twin.getDeclaredReturnType());
        }
        if(original.getNumParams() != twin.getNumParams()) {
            add(where, "parameter count differs");
            return;
        }
        const auto* left = original.getType()->getAs<clang::FunctionProtoType>();
        const auto* right = twin.getType()->getAs<clang::FunctionProtoType>();
        if(left && right) {
            flag(where, "...", left->isVariadic(), right->isVariadic());
            if(left->getMethodQuals() != right->getMethodQuals() ||
               left->getRefQualifier() != right->getRefQualifier()) {
                add(where, "qualifiers differ");
            }
            if(left->getExceptionSpecType() != right->getExceptionSpecType() ||
               !same_expr(left->getNoexceptExpr(), right->getNoexceptExpr())) {
                add(where, "exception specification differs");
            }
        } else if(!left != !right) {
            add(where, "prototype differs");
        }
        for(unsigned index = 0; index < original.getNumParams(); index += 1) {
            const auto* param = original.getParamDecl(index);
            const auto* other = twin.getParamDecl(index);
            auto name = std::format("parameter {}", index + 1);
            same_type(where, name, param->getOriginalType(), other->getOriginalType());
            if(param->hasUnparsedDefaultArg() || other->hasUnparsedDefaultArg()) {
                continue;
            }
            const auto* value = param->hasUninstantiatedDefaultArg()
                                    ? param->getUninstantiatedDefaultArg()
                                    : (param->hasDefaultArg() ? param->getDefaultArg() : nullptr);
            const auto* other_value =
                other->hasUninstantiatedDefaultArg()
                    ? other->getUninstantiatedDefaultArg()
                    : (other->hasDefaultArg() ? other->getDefaultArg() : nullptr);
            if(!value || !other_value) {
                flag(where, name + " default", value, other_value);
            } else if(!same_expr(value, other_value)) {
                add(where, name + " default differs");
            }
        }
        if(original.getStorageClass() != twin.getStorageClass()) {
            add(where, "storage class differs");
        }
        flag(where, "inline", original.isInlineSpecified(), twin.isInlineSpecified());
        if(original.getConstexprKind() != twin.getConstexprKind()) {
            add(where, "constexpr differs");
        }
        const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(&original);
        if(!method || method->size_overridden_methods() == 0) {
            flag(where, "virtual", original.isVirtualAsWritten(), twin.isVirtualAsWritten());
        }
        flag(where, "= 0", original.isPureVirtual(), twin.isPureVirtual());
        flag(where, "= delete", original.isDeletedAsWritten(), twin.isDeletedAsWritten());
        flag(where, "= default", original.isExplicitlyDefaulted(), twin.isExplicitlyDefaulted());
        const auto& left_requires = original.getTrailingRequiresClause();
        const auto& right_requires = twin.getTrailingRequiresClause();
        if(!same_expr(left_requires.ConstraintExpr, right_requires.ConstraintExpr)) {
            add(where, "requires clause differs");
        }
    }

    void enumeration(llvm::StringRef where,
                     const clang::EnumDecl& original,
                     const clang::EnumDecl& twin) {
        flag(where, "scoped", original.isScoped(), twin.isScoped());
        flag(where, "fixed type", original.isFixed(), twin.isFixed());
        if(original.isFixed() && twin.isFixed()) {
            same_type(where, "underlying type", original.getIntegerType(), twin.getIntegerType());
        }
        auto originals = original.enumerators();
        auto it = originals.begin();
        for(const auto* enumerator: twin.enumerators()) {
            if(it == originals.end()) {
                add(where, std::format("enumerator {} added", enumerator->getName()));
                break;
            }
            auto name = enumerator->getName();
            if(name.consume_front(twin_prefix)) {
                name = name.split('_').second;
            }
            if(name != (*it)->getName()) {
                add(where, std::format("enumerator {}, twin {}", (*it)->getName(), name));
            } else if(!llvm::APSInt::isSameValue(enumerator->getInitVal(), (*it)->getInitVal())) {
                add(where, std::format("enumerator {} value differs", name));
            }
            ++it;
        }
    }

    /// The type a member declares with its declarator: a variable's, a
    /// field's or an alias's, through pointers, references and arrays.
    auto tag_of(const clang::Decl& member) -> const clang::TagDecl* {
        clang::QualType type;
        if(const auto* value = llvm::dyn_cast<clang::ValueDecl>(&member)) {
            type = value->getType();
        } else if(const auto* alias = llvm::dyn_cast<clang::TypedefNameDecl>(&member)) {
            type = alias->getUnderlyingType();
        } else {
            return nullptr;
        }
        while(true) {
            if(const auto* array = context.getAsArrayType(type)) {
                type = array->getElementType();
            } else if(type->isPointerType() || type->isReferenceType()) {
                type = type->getPointeeType();
            } else {
                break;
            }
        }
        return type->getAsTagDecl();
    }

    /// The named member of `record` declared with the unnamed `tag`.
    auto user_of(const clang::RecordDecl& record, const clang::TagDecl& tag)
        -> const clang::NamedDecl* {
        for(const auto* member: record.decls()) {
            const auto* named = llvm::dyn_cast<clang::NamedDecl>(member);
            if(const auto* used = tag_of(*member);
               named && named->getDeclName() && used &&
               used->getCanonicalDecl() == tag.getCanonicalDecl()) {
                return named;
            }
        }
        return nullptr;
    }

    void record(llvm::StringRef where,
                const clang::RecordDecl& original,
                const clang::RecordDecl& twin) {
        matched[twin.getCanonicalDecl()] = original.getCanonicalDecl();
        if(original.getTagKind() != twin.getTagKind()) {
            add(where, "class key differs");
        }
        const auto* left = llvm::dyn_cast<clang::CXXRecordDecl>(&original);
        const auto* right = llvm::dyn_cast<clang::CXXRecordDecl>(&twin);
        if(left && right && left->hasDefinition() && right->hasDefinition()) {
            if(instantiation && where.empty()) {
                for(const auto& other: right->bases()) {
                    if(llvm::none_of(left->bases(), [&](const clang::CXXBaseSpecifier& base) {
                           return same_type(base.getType(), other.getType()) &&
                                  base.isVirtual() == other.isVirtual();
                       })) {
                        add(where,
                            std::format("base `{}` not in the original", spell(other.getType())));
                    }
                }
            } else if(left->getNumBases() != right->getNumBases()) {
                add(where, "base count differs");
            } else {
                for(auto [base, other]: llvm::zip(left->bases(), right->bases())) {
                    same_type(where, "base", base.getType(), other.getType());
                    flag(where, "virtual base", base.isVirtual(), other.isVirtual());
                }
            }
        }
        if(!twin.isCompleteDefinition()) {
            return;
        }

        // An unnamed type is matched through the member declared with it,
        // `shared` of `static struct { ... } shared;`, the others by their
        // order.
        llvm::SmallVector<const clang::TagDecl*> unnamed;
        for(const auto* member: original.decls()) {
            if(const auto* tag = llvm::dyn_cast<clang::TagDecl>(member);
               tag && !tag->getDeclName() && !tag->isImplicit() && !user_of(original, *tag)) {
                unnamed.push_back(tag);
            }
        }
        std::size_t next_unnamed = 0;
        llvm::SmallVector<std::pair<const clang::Decl*, const clang::Decl*>> pairs;
        for(const auto* member: twin.decls()) {
            if(member->isImplicit()) {
                continue;
            }
            const clang::Decl* counterpart = nullptr;
            const auto* named = llvm::dyn_cast<clang::NamedDecl>(member);
            if(named && named->getDeclName()) {
                for(const auto* candidate: original.lookup(named->getDeclName())) {
                    if(candidate->getKind() == member->getKind() && !candidate->isImplicit()) {
                        counterpart = candidate->getCanonicalDecl();
                        if(const auto* tag = llvm::dyn_cast<clang::TagDecl>(candidate);
                           tag && tag->getDefinition()) {
                            counterpart = tag->getDefinition();
                        }
                        break;
                    }
                }
            } else if(const auto* tag = llvm::dyn_cast<clang::TagDecl>(member)) {
                if(const auto* user = user_of(twin, *tag)) {
                    for(const auto* candidate: original.lookup(user->getDeclName())) {
                        if(const auto* written = tag_of(*candidate)) {
                            counterpart =
                                written->getDefinition() ? written->getDefinition() : written;
                            break;
                        }
                    }
                } else if(next_unnamed < unnamed.size()) {
                    counterpart = unnamed[next_unnamed];
                    next_unnamed += 1;
                } else {
                    continue;
                }
            } else {
                continue;
            }
            if(!counterpart) {
                add(where,
                    std::format("member {} not in the original",
                                named ? named->getName() : "(unnamed)"));
                continue;
            }
            if(const auto* tag = llvm::dyn_cast<clang::TagDecl>(member)) {
                matched[tag->getCanonicalDecl()] = counterpart->getCanonicalDecl();
            }
            if(const auto* tmpl = llvm::dyn_cast<clang::ClassTemplateDecl>(member)) {
                matched[tmpl->getCanonicalDecl()] = counterpart->getCanonicalDecl();
                matched[tmpl->getTemplatedDecl()->getCanonicalDecl()] =
                    llvm::cast<clang::ClassTemplateDecl>(counterpart)
                        ->getTemplatedDecl()
                        ->getCanonicalDecl();
            }
            pairs.emplace_back(counterpart, member);
        }
        for(auto [counterpart, member]: pairs) {
            std::string inner = where.str();
            if(const auto* named = llvm::dyn_cast<clang::NamedDecl>(member);
               named && named->getDeclName()) {
                inner += "::" + named->getName().str();
            } else {
                inner += "::(unnamed)";
            }
            if(const auto* tag = llvm::dyn_cast<clang::TagDecl>(member);
               tag && !tag->isCompleteDefinition()) {
                continue;
            }
            if(counterpart->getAccess() != member->getAccess()) {
                add(inner, "access differs");
            }
            compare(inner, *counterpart, *member);
        }
    }

    void compare(llvm::StringRef where, const clang::Decl& original, const clang::Decl& twin) {
        // A specialization's twin is a class of its own.
        bool records = llvm::isa<clang::RecordDecl>(original) && llvm::isa<clang::RecordDecl>(twin);
        if(original.getKind() != twin.getKind() && !records) {
            add(where,
                std::format("declares a {}, not a {}",
                            twin.getDeclKindName(),
                            original.getDeclKindName()));
            return;
        }
        attributes(where, original, twin);
        if(const auto* tmpl = llvm::dyn_cast<clang::TemplateDecl>(&original)) {
            const auto& other = llvm::cast<clang::TemplateDecl>(twin);
            template_params(where, tmpl->getTemplateParameters(), other.getTemplateParameters());
            if(const auto* concept_decl = llvm::dyn_cast<clang::ConceptDecl>(tmpl)) {
                if(!same_expr(concept_decl->getConstraintExpr(),
                              llvm::cast<clang::ConceptDecl>(other).getConstraintExpr())) {
                    add(where, "constraint differs");
                }
                return;
            }
            if(tmpl->getTemplatedDecl() && other.getTemplatedDecl()) {
                compare(where, *tmpl->getTemplatedDecl(), *other.getTemplatedDecl());
            }
            return;
        }
        if(records && !(templated_record && where.empty())) {
            const clang::TemplateParameterList* left = nullptr;
            const clang::TemplateParameterList* right = nullptr;
            if(const auto* partial =
                   llvm::dyn_cast<clang::ClassTemplatePartialSpecializationDecl>(&original)) {
                left = partial->getTemplateParameters();
            } else if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(&original);
                      record && record->getDescribedClassTemplate()) {
                left = record->getDescribedClassTemplate()->getTemplateParameters();
            }
            if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(&twin);
               record && record->getDescribedClassTemplate()) {
                right = record->getDescribedClassTemplate()->getTemplateParameters();
            }
            if(left || right) {
                template_params(where, left, right);
            }
        }
        if(const auto* var = llvm::dyn_cast<clang::VarDecl>(&original)) {
            variable(where, *var, llvm::cast<clang::VarDecl>(twin));
        } else if(const auto* field_decl = llvm::dyn_cast<clang::FieldDecl>(&original)) {
            field(where, *field_decl, llvm::cast<clang::FieldDecl>(twin));
        } else if(const auto* function_decl = llvm::dyn_cast<clang::FunctionDecl>(&original)) {
            function(where, *function_decl, llvm::cast<clang::FunctionDecl>(twin));
        } else if(const auto* alias = llvm::dyn_cast<clang::TypedefNameDecl>(&original)) {
            same_type(where,
                      "aliased type",
                      alias->getUnderlyingType(),
                      llvm::cast<clang::TypedefNameDecl>(twin).getUnderlyingType());
        } else if(const auto* enum_decl = llvm::dyn_cast<clang::EnumDecl>(&original)) {
            enumeration(where, *enum_decl, llvm::cast<clang::EnumDecl>(twin));
        } else if(const auto* record_decl = llvm::dyn_cast<clang::RecordDecl>(&original)) {
            record(where, *record_decl, llvm::cast<clang::RecordDecl>(twin));
        }
    }
};

/// Every declaration of the second compile, by where its name is, and the
/// twins by name.
struct ProbeIndex : clang::RecursiveASTVisitor<ProbeIndex> {
    clang::SourceManager& sources;
    CompilationUnit& unit;
    std::map<Place, std::vector<const clang::NamedDecl*>> placed;
    llvm::StringMap<const clang::NamedDecl*> twins;

    ProbeIndex(CompilationUnit& unit) : sources(unit.context().getSourceManager()), unit(unit) {}

    bool shouldVisitTemplateInstantiations() const {
        return false;
    }

    bool VisitNamedDecl(clang::NamedDecl* decl) {
        if(decl->isImplicit()) {
            return true;
        }
        if(auto* identifier = decl->getIdentifier();
           identifier && identifier->getName().starts_with(twin_prefix)) {
            // The templated declaration stands for its template.
            const clang::NamedDecl* twin = decl;
            if(const auto* tmpl = llvm::dyn_cast<clang::TemplateDecl>(decl);
               tmpl && tmpl->getTemplatedDecl()) {
                twin = tmpl->getTemplatedDecl();
            }
            twins.try_emplace(identifier->getName(), twin);
        }
        auto location = decl->getLocation();
        if(location.isValid() && location.isFileID()) {
            auto [fid, offset] = sources.getDecomposedLoc(location);
            placed[{unit.file_path(fid).str(), offset}].push_back(decl);
        }
        return true;
    }
};

auto place_of(CompilationUnit& unit, clang::SourceLocation location) -> std::optional<Place> {
    if(location.isInvalid() || !location.isFileID()) {
        return std::nullopt;
    }
    auto [fid, offset] = unit.context().getSourceManager().getDecomposedLoc(location);
    return Place{unit.file_path(fid).str(), offset};
}

auto printed_args(const clang::ClassTemplateSpecializationDecl& spec) -> std::string {
    std::string text;
    llvm::raw_string_ostream os(text);
    auto policy = spec.getASTContext().getPrintingPolicy();
    policy.PrintAsCanonical = true;
    clang::printTemplateArgumentList(os, spec.getTemplateArgs().asArray(), policy);
    return text;
}

auto line_of(CompilationUnit& unit, clang::SourceLocation location) -> std::string {
    auto presumed = unit.context().getSourceManager().getPresumedLoc(
        unit.context().getSourceManager().getFileLoc(location));
    if(presumed.isInvalid()) {
        return "";
    }
    return std::format("{}:{}", presumed.getFilename(), presumed.getLine());
}

/// Check the definitions hover cards print for the declarations of every
/// file the compile reads: each card's definition, its declaration given a
/// name of its own, is compiled again right after the declaration it shows,
/// and the two declarations are compared — their types, specifiers,
/// attributes, initializers and, for a class or an enum, the members the
/// card lists. A card of an implicit instantiation is compiled at the end
/// of the main file.
auto check_hover_definitions(const CompilationInput& input,
                             llvm::StringRef main_path,
                             const feature::HoverOptions& options) -> HoverCheckReport {
    HoverCheckReport report;
    // Warnings in system headers too: a twin's in one is checked as any.
    CompilationInput first_input = input;
    first_input.arguments.push_back("-Wsystem-headers");
    // Every twin's errors, however many the twins draw.
    first_input.arguments.push_back("-ferror-limit");
    first_input.arguments.push_back("0");
    // A twin uses what its original uses.
    first_input.arguments.push_back("-Wno-deprecated-declarations");
    auto unit = compile({.kind = CompilationKind::Content, .input = first_input});
    // The input compiled once already, for the clean-code gate.
    assert(unit->completed());
    auto& context = unit->context();

    Collector collector;
    collector.TraverseAST(context);

    std::vector<Twin> twins;
    auto add = [&](const clang::NamedDecl& decl, bool instantiation) {
        Twin twin;
        twin.name = std::format("{}{}", twin_prefix, twins.size());
        twin.original = &decl;
        twin.instantiation = instantiation;
        twin.finding.instantiation = instantiation;
        twin.finding.name = decl.getQualifiedNameAsString();
        twin.finding.location = line_of(*unit, decl.getLocation());
        LambdaBodies lambdas{.context = context};
        lambdas.TraverseDecl(const_cast<clang::NamedDecl*>(&decl));
        twin.lambda_bodies = std::move(lambdas.bodies);

        feature::HoverInfo info;
        if(instantiation) {
            const auto& spec = llvm::cast<clang::ClassTemplateSpecializationDecl>(decl);
            const auto* pattern = spec.getSpecializedTemplate();
            auto place = place_of(*unit, pattern->getLocation());
            auto scopes = reopened(*pattern);
            if(!place || !scopes) {
                return;
            }
            twin.place = *place;
            twin.head_name = spec.getName().str();
            twin.arguments = printed_args(spec);
            twin.finding.name += twin.arguments;
            info = feature::hover_info(*unit, decl, options);
            twin.finding.definition = info.definition;
            // A lambda's type has no name to write.
            if(llvm::StringRef(info.definition).contains("(lambda)")) {
                return;
            }
            auto main = unit->context().getSourceManager().getMainFileID();
            twin.insertions.push_back({
                .at = {unit->file_path(main).str(),
                       static_cast<std::uint32_t>(unit->file_content(main).size())},
                .text = std::format("\n{}{}\n{}\n",
                                    scopes->first,
                                    twin_text(info.definition, twin, false),
                                    scopes->second)
            });
        } else {
            auto place = place_of(*unit, decl.getLocation());
            auto end = declaration_end(*unit, decl);
            if(!place || !end) {
                return;
            }
            twin.place = *place;
            const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(&decl);
            twin.templated_record =
                record && (record->getDescribedClassTemplate() ||
                           llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(record));
            if(record && (twin.templated_record ||
                          llvm::isa<clang::ClassTemplateSpecializationDecl>(record))) {
                twin.head_name = record->getName().str();
            }
            {
                Renaming renaming{.twin_name = twin.name};
                renaming.rename_declaration(decl, context);
                info = feature::hover_info(*unit, decl, options);
            }
            twin.finding.definition = info.definition;
            if(!llvm::StringRef(info.definition)
                    .contains(twin.head_name.empty() ? twin.name : twin.head_name)) {
                return;
            }
            // A class's twin is nested in it, where the names the card uses
            // are found as its members find them; C has no class scopes.
            if(record && context.getLangOpts().CPlusPlus) {
                auto brace = place_of(*unit, record->getBraceRange().getEnd());
                if(!brace) {
                    return;
                }
                twin.insertions.push_back(
                    {.at = *brace, .text = "\n" + twin_text(info.definition, twin, true) + "\n"});
                if(twin.templated_record) {
                    llvm::StringRef definition = info.definition;
                    auto key = class_key(definition, twin.head_name);
                    if(key == llvm::StringRef::npos) {
                        return;
                    }
                    auto template_head = definition.take_front(key);
                    twin.insertions.push_back({
                        .at = {place->path, *end},
                        .text = std::format("\n{}void {}_head();\n", template_head, twin.name)
                    });
                }
            } else if(const auto* field = llvm::dyn_cast<clang::FieldDecl>(&decl)) {
                // A field's twin goes in a class of its own: in the field's,
                // it would change how the class is laid out and initialized.
                // An anonymous struct or union holds fields alone, and C
                // declares a class at file scope wherever it is written.
                const clang::RecordDecl* outer = field->getParent();
                while((outer->isAnonymousStructOrUnion() || !context.getLangOpts().CPlusPlus) &&
                      llvm::isa<clang::RecordDecl>(outer->getDeclContext())) {
                    outer = llvm::cast<clang::RecordDecl>(outer->getDeclContext());
                }
                if(!context.getLangOpts().CPlusPlus) {
                    end = declaration_end(*unit, *outer);
                } else if(outer != field->getParent()) {
                    end = place_of(*unit, outer->getBraceRange().getEnd())
                              .transform([](const Place& brace) { return brace.offset; });
                }
                if(!end) {
                    return;
                }
                twin.insertions.push_back({
                    .at = {place->path, *end},
                    .text = std::format("\nstruct {}_scope {{\n{}\n}};\n",
                                        twin.name,
                                        twin_text(info.definition, twin, false))
                });
            } else {
                twin.insertions.push_back({
                    .at = {place->path, *end},
                    .text = "\n" + twin_text(info.definition, twin, false) + "\n"
                });
            }
        }
        twins.push_back(std::move(twin));
    };
    for(const auto* decl: collector.decls) {
        add(*decl, false);
    }
    for(const auto* spec: collector.instantiations) {
        add(*spec, true);
    }
    report.checked = twins.size();

    // A file included twice is written once.
    std::set<std::pair<std::string, std::uint32_t>> seen;
    std::erase_if(twins, [&](const Twin& twin) {
        return !twin.instantiation && !seen.emplace(twin.place.path, twin.place.offset).second;
    });

    // A twin with a directive, or brackets its comments leave unbalanced,
    // can break the rest of the compile: it is reported, never inserted.
    std::vector<Twin*> pending;
    for(auto& twin: twins) {
        std::optional<std::string> broken;
        for(const auto& insertion: twin.insertions) {
            clang::Lexer lexer(clang::SourceLocation(),
                               unit->lang_options(),
                               insertion.text.data(),
                               insertion.text.data(),
                               insertion.text.data() + insertion.text.size());
            std::vector<clang::tok::TokenKind> open;
            clang::Token token;
            while(!broken && !lexer.LexFromRawLexer(token)) {
                auto kind = token.getKind();
                if(kind == clang::tok::hash && token.isAtStartOfLine()) {
                    broken = "a preprocessor directive in the definition";
                } else if(kind == clang::tok::l_brace || kind == clang::tok::l_paren ||
                          kind == clang::tok::l_square) {
                    open.push_back(kind);
                } else if(kind == clang::tok::r_brace || kind == clang::tok::r_paren ||
                          kind == clang::tok::r_square) {
                    auto expected = kind == clang::tok::r_brace   ? clang::tok::l_brace
                                    : kind == clang::tok::r_paren ? clang::tok::l_paren
                                                                  : clang::tok::l_square;
                    if(open.empty() || open.back() != expected) {
                        broken = "unbalanced brackets in the definition";
                    } else {
                        open.pop_back();
                    }
                }
            }
            if(!broken && !open.empty()) {
                broken = "unbalanced brackets in the definition";
            }
        }
        if(broken) {
            twin.finding.problems.push_back("error: " + *broken);
            report.findings.push_back(std::move(twin.finding));
        } else {
            pending.push_back(&twin);
        }
    }

    // A compile with every twin; a twin clang's recovery from another
    // one's errors swallowed goes in another, with the others it lost, up to
    // three rounds.
    std::set<std::string> known;
    for(const auto& diagnostic: unit->diagnostics()) {
        known.insert(diagnostic.message);
    }
    auto probe_round = [&](llvm::ArrayRef<Twin*> round) -> std::vector<Twin*> {
        // The files with their twins inserted, in order of where they go.
        std::vector<Twin::Insertion*> ordered;
        for(auto* twin: round) {
            for(auto& insertion: twin->insertions) {
                ordered.push_back(&insertion);
            }
        }
        std::ranges::stable_sort(ordered,
                                 [](const Twin::Insertion* left, const Twin::Insertion* right) {
                                     return std::tie(left->at.path, left->at.offset) <
                                            std::tie(right->at.path, right->at.offset);
                                 });
        llvm::StringMap<llvm::StringRef> originals;
        for(auto* insertion: ordered) {
            if(!originals.contains(insertion->at.path)) {
                originals[insertion->at.path] =
                    unit->file_content(unit->file_id(insertion->at.path));
            }
        }
        llvm::StringMap<std::string> contents;
        llvm::StringMap<std::uint32_t> copied;
        /// Per file, the original offsets inserted at and the text's length,
        /// to find a declaration in the second compile.
        llvm::StringMap<std::vector<std::pair<std::uint32_t, std::uint32_t>>> shifts;
        for(auto* insertion: ordered) {
            auto text = originals[insertion->at.path];
            auto& content = contents[insertion->at.path];
            auto& from = copied[insertion->at.path];
            content += text.substr(from, insertion->at.offset - from);
            from = insertion->at.offset;
            insertion->begin = content.size();
            content += insertion->text;
            insertion->end = content.size();
            shifts[insertion->at.path].emplace_back(insertion->at.offset, insertion->text.size());
        }
        for(auto& [path, content]: contents) {
            content += originals[path].substr(copied[path]);
        }

        CompilationInput probe_input = input;
        auto main_spelling = unit->file_path(unit->context().getSourceManager().getMainFileID());
        for(auto& [path, content]: contents) {
            probe_input.buffers[path == main_spelling ? main_path : path] = content;
        }
        probe_input.arguments = first_input.arguments;
        auto probe = compile({.kind = CompilationKind::Content, .input = probe_input});
        // A twin the compile stops at leaves all of them unmatched.
        if(!probe->completed()) {
            return round.vec();
        }

        auto& probe_context = probe->context();
        for(const auto& diagnostic: probe->diagnostics()) {
            if(diagnostic.id.level < DiagnosticLevel::Warning || diagnostic.fid.isInvalid() ||
               (diagnostic.id.level == DiagnosticLevel::Warning &&
                known.contains(diagnostic.message))) {
                continue;
            }
            auto path = probe->file_path(diagnostic.fid);
            for(auto* pointer: round) {
                auto& twin = *pointer;
                bool inside = llvm::any_of(twin.insertions, [&](const Twin::Insertion& insertion) {
                    return insertion.at.path == path && insertion.begin <= diagnostic.range.begin &&
                           diagnostic.range.begin < insertion.end;
                });
                if(!inside) {
                    continue;
                }
                // An instantiation's twin is outside its class.
                if(twin.instantiation && (diagnostic.message.contains("is a private member") ||
                                          diagnostic.message.contains("is a protected member"))) {
                    break;
                }
                // A card leaves out member functions: its class can be abstract,
                // or lack the constructor its reference members need.
                if(diagnostic.message.contains("abstract class is marked") ||
                   diagnostic.message.contains("does not declare any constructor to initialize")) {
                    break;
                }
                // A renamed override overrides nothing.
                const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(twin.original);
                if(method &&
                   (method->size_overridden_methods() != 0 ||
                    method->hasAttr<clang::OverrideAttr>()) &&
                   (diagnostic.message.contains("override") ||
                    diagnostic.message.contains("only virtual member functions"))) {
                    break;
                }
                twin.finding.problems.push_back(std::format(
                    "{}: {}",
                    diagnostic.id.level == DiagnosticLevel::Warning ? "warning" : "error",
                    diagnostic.message));
                break;
            }
        }

        ProbeIndex index(*probe);
        index.TraverseAST(probe_context);
        auto shifted = [&](const Place& place) {
            Place result = place;
            for(auto [at, length]: shifts.lookup(place.path)) {
                if(at <= place.offset) {
                    result.offset += length;
                }
            }
            return result;
        };
        std::vector<Twin*> lost;
        for(auto* pointer: round) {
            auto& twin = *pointer;
            const clang::NamedDecl* original = nullptr;
            auto found = index.placed.find(shifted(twin.place));
            if(found != index.placed.end()) {
                for(const auto* candidate: found->second) {
                    if(twin.instantiation) {
                        if(const auto* tmpl = llvm::dyn_cast<clang::ClassTemplateDecl>(candidate)) {
                            for(const auto* spec: tmpl->specializations()) {
                                if(printed_args(*spec) == twin.arguments) {
                                    original = spec;
                                }
                            }
                        }
                    } else if(candidate->getKind() == twin.original->getKind()) {
                        original = candidate;
                    }
                }
            }
            const auto* declared = index.twins.lookup(twin.name);
            if(!original || !declared) {
                if(twin.finding.problems.empty()) {
                    lost.push_back(&twin);
                }
            } else {
                Comparison comparison{.context = probe_context,
                                      .problems = twin.finding.problems,
                                      .instantiation = twin.instantiation,
                                      .templated_record = twin.templated_record};
                comparison.compare("", *original, *declared);
                if(const auto* record = llvm::dyn_cast<clang::RecordDecl>(original)) {
                    for(const auto& name: twin.member_types) {
                        auto found = record->lookup(&probe_context.Idents.get(name));
                        if(!llvm::any_of(found, [](const clang::NamedDecl* decl) {
                               return llvm::isa<clang::TagDecl, clang::ClassTemplateDecl>(decl);
                           })) {
                            twin.finding.problems.push_back(
                                std::format("member type {} not in the original", name));
                        }
                    }
                }
                if(twin.templated_record) {
                    const auto* head = index.twins.lookup(twin.name + "_head");
                    const auto* params =
                        llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(original)
                            ? llvm::cast<clang::ClassTemplatePartialSpecializationDecl>(original)
                                  ->getTemplateParameters()
                            : llvm::cast<clang::CXXRecordDecl>(original)
                                  ->getDescribedClassTemplate()
                                  ->getTemplateParameters();
                    const auto* described =
                        head ? llvm::cast<clang::FunctionDecl>(head)->getDescribedFunctionTemplate()
                             : nullptr;
                    if(!described) {
                        twin.finding.problems.push_back("template head not declared");
                    } else {
                        comparison.template_params("template head",
                                                   params,
                                                   described->getTemplateParameters());
                    }
                }
            }
            if(!twin.finding.problems.empty()) {
                report.findings.push_back(std::move(twin.finding));
            }
        }
        return lost;
    };
    for(int round = 0; round < 3 && !pending.empty(); round += 1) {
        auto lost = probe_round(pending);
        if(lost.size() == pending.size()) {
            break;
        }
        pending = std::move(lost);
    }
    report.unmatched = pending.size();
    return report;
}

/// Every hover card of the file's declarations whose definition does not
/// declare the same thing again (check_hover_definitions).
std::optional<kota::codec::RawValue> run_hover_check(const CompilationInput& input,
                                                     llvm::StringRef path,
                                                     const Config& config) {
    return to_raw_json(check_hover_definitions(input, path, config.hover));
}

/// Completion-shaped features (code completion, signature help) drive
/// their own compilation: the offset parameterizes the parse itself, so
/// each point gets a fresh completion compile instead of a query
/// against one shared unit. `params` arrives fully configured except for
/// the completion offset.
std::optional<kota::codec::RawValue> run_code_completion(CompilationParams params,
                                                         const Config& config) {
    // The replies of an editor client that takes everything an item can carry.
    return to_raw_json(
        feature::code_complete(std::move(params),
                               config.code_completion,
                               config.documentation,
                               {.snippets = true, .insert_replace = true, .markdown = true})
            .list);
}

std::optional<kota::codec::RawValue> run_signature_help(CompilationParams params,
                                                        const Config& config) {
    return to_raw_json(
        feature::signature_help(std::move(params), config.signature_help, config.documentation));
}

/// Marker payload for document_highlight: the unit's own TU index stands
/// in for the server's file index, the cursor resolved by the same rule.
std::optional<kota::codec::RawValue> run_document_highlight(CompilationUnit& unit,
                                                            std::uint32_t offset,
                                                            [[maybe_unused]] const Config& config) {
    auto bytes = index::build_tu_index(unit);
    auto tu = index::TUIndex::from_bytes(bytes);
    const index::Shard& shard = tu.shard_of(tu.path_count() - 1);
    auto named = index::named_at(shard, offset, [&](index::SymbolHash hash) {
        auto symbol = tu.find_symbol(hash);
        return symbol && symbol->kind == SymbolKind::Macro;
    });
    if(!named) {
        return to_raw_json(std::vector<index::Highlight>{});
    }
    return to_raw_json(index::highlights({&shard}, named->symbols));
}

/// A feature runs in exactly one shape: whole-document (`run`), once per
/// point against a shared unit (`run_at`), once per range with a
/// whole-document default (`run_over`), once per point (an empty
/// selection) and per range with no default (`run_select`), once per
/// point with its own completion compile (`run_complete`), or over
/// compiles of its own (`run_compiles`). `check` is what --check runs
/// instead, for a feature that checks its own results.
struct FeatureSpec {
    llvm::StringRef name;
    llvm::StringRef description;
    std::optional<kota::codec::RawValue> (*run)(CompilationUnit&, const Config&) = nullptr;
    std::optional<kota::codec::RawValue> (*run_at)(CompilationUnit&,
                                                   std::uint32_t,
                                                   const Config&) = nullptr;
    std::optional<kota::codec::RawValue> (*run_over)(CompilationUnit&,
                                                     LocalSourceRange,
                                                     const Config&) = nullptr;
    std::optional<kota::codec::RawValue> (*run_select)(CompilationUnit&,
                                                       LocalSourceRange,
                                                       const Config&) = nullptr;
    std::optional<kota::codec::RawValue> (*run_complete)(CompilationParams,
                                                         const Config&) = nullptr;
    std::optional<kota::codec::RawValue> (*run_compiles)(const CompilationInput&,
                                                         llvm::StringRef,
                                                         const Config&) = nullptr;
    const FeatureSpec* check = nullptr;
};

constexpr FeatureSpec document_symbols_check{.name = "document-symbols",
                                             .run = run_document_symbol_check};
constexpr FeatureSpec hover_check{.name = "hover", .run_compiles = run_hover_check};

constexpr std::array features = {
    FeatureSpec{.name = "code-action",
                .description = "The code actions on a selection",
                .run_select = run_code_action},
    FeatureSpec{.name = "code-completion",
                .description = "The completions at a point",
                .run_complete = run_code_completion},
    FeatureSpec{.name = "document-highlight",
                .description = "The places naming the symbol at a point",
                .run_at = run_document_highlight},
    FeatureSpec{.name = "document-links",
                .description = "The file's links",
                .run = run_document_links},
    FeatureSpec{.name = "document-symbols",
                .description = "The file's outline",
                .run = run_document_symbols,
                .check = &document_symbols_check},
    FeatureSpec{.name = "folding-range",
                .description = "The file's folding ranges",
                .run = run_folding_ranges},
    FeatureSpec{.name = "hover",
                .description = "The hover card at a point",
                .run_at = run_hover,
                .check = &hover_check},
    FeatureSpec{.name = "inlay-hint",
                .description = "The inlay hints over a range, else the file",
                .run_over = run_inlay_hints},
    FeatureSpec{.name = "selection-range",
                .description = "The selection ranges at a point",
                .run_at = run_selection_range},
    FeatureSpec{.name = "semantic-tokens",
                .description = "The file's semantic tokens",
                .run = run_semantic_tokens},
    FeatureSpec{.name = "signature-help",
                .description = "The signature help at a point",
                .run_complete = run_signature_help},
};

/// The inspected file, with the markers --markers places in it.
struct SourceFile {
    std::string path;
    std::string content;
    FileMarkers markers;
};

/// The compile command for one file.
struct FileCommand {
    std::vector<std::string> arguments;
    std::string directory;
    std::string workspace;
    std::shared_ptr<const SynthesizedContext> synthesized;
};

/// The workspace the inspected file's directory belongs to: itself or the
/// nearest ancestor holding a configuration file or a compile_commands.json
/// (the upward lookup clangd does), so a nested tree resolves its commands
/// as the server would from the project root; `start` itself when none does.
/// Only the ancestors themselves are checked — scanning their
/// subdirectories would let an unrelated sibling project's database win.
CanonicalPath workspace_of(CanonicalRef start) {
    CanonicalPath workspace = start;
    path::walk_ancestors(start, [&](CanonicalRef dir) {
        bool marked = llvm::any_of(config_file_names,
                                   [&](llvm::StringRef marker) {
                                       return vfs::exists(path::join(dir, marker));
                                   }) ||
                      vfs::exists(path::join(dir, "compile_commands.json"));
        if(marked) {
            workspace = dir;
        }
        return !marked;
    });
    return workspace;
}

/// The compile command for `file`: explicit --flags arguments, else the
/// resolution the server performs for a background compile: the file's
/// entry, a host's command for a header, a rule's default command, the
/// builtin fallback. On failure records the error on `entry` and returns
/// nullopt.
std::optional<FileCommand> file_command(FileEntry& entry,
                                        const std::string& file,
                                        llvm::ArrayRef<std::string> flags,
                                        llvm::StringRef flags_directory,
                                        FileTable& files,
                                        CommandResolver& commands) {
    namespace types = clang::driver::types;
    auto type = suffix_type(file);
    bool is_header = type == types::TY_CHeader || type == types::TY_CXXHeader;

    FileCommand command;
    if(!flags.empty()) {
        bool is_cxx = type != types::TY_INVALID && types::isCXX(type);
        std::vector<const char*> driver_args = {is_cxx || is_header ? "clang++" : "clang"};
        if(is_header) {
            // An ambiguous header is C++ by default, like clangd; -x forces
            // TU semantics instead of a precompiled-header job.
            driver_args.insert(driver_args.end(), {"-x", "c++"});
        }
        // The toolchain query spawns the driver from the process cwd, and the
        // driver reads @response-files itself — resolve them against the
        // file's directory so they don't depend on where inspect was launched.
        std::vector<std::string> resolved_flags(flags.begin(), flags.end());
        for(auto& flag: resolved_flags) {
            llvm::StringRef rest(flag);
            if(rest.consume_front("@") && !path::is_absolute(rest)) {
                llvm::SmallString<256> abs(flags_directory);
                path::append(abs, rest);
                flag = ("@" + abs).str();
            }
        }
        for(auto& flag: resolved_flags) {
            driver_args.push_back(flag.c_str());
        }
        driver_args.insert(driver_args.end(), {"-fsyntax-only", file.c_str()});
        auto cc1 = Toolchain::query(driver_args, file);
        if(!cc1) {
            entry.error = "toolchain_error";
            entry.diagnostics = {std::move(cc1.error())};
            return std::nullopt;
        }
        command.arguments = std::move(*cc1);
        command.directory = flags_directory.str();
        return command;
    }

    command.synthesized = commands
                              .resolve_command(files.intern(Spelling::absolute(file)),
                                               command.directory,
                                               command.arguments)
                              .synthesized;
    return command;
}

/// Run the feature over the file and fill its entry. The compile is one
/// pass, deliberately without the preamble PCH the server uses: a shared
/// snapshot pins that the PCH split does not change feature results, so any
/// divergence between the two paths shows as a snapshot mismatch instead of
/// hiding in the preamble. With `diagnostics.clang_tidy` the compile runs
/// the file's clang-tidy configuration as the server's does.
void run_feature(FileEntry& entry,
                 const FeatureSpec& spec,
                 const SourceFile& file,
                 const FileCommand& command,
                 const Config& config) {
    CompilationInput input{
        .arguments = command.arguments,
        .directory = command.directory,
        .workspace = command.workspace,
    };
    std::string text = file.content;
    if(command.synthesized) {
        input.add_synthesized(command.synthesized->files);
        command.synthesized->append_suffix_include(text);
    }
    input.add_remapped_file(file.path, text);

    CompilationParams params{.kind = CompilationKind::Content, .input = input};
    if(config.diagnostics.clang_tidy) {
        auto resolution = tidy::resolve_tidy_params(file.path);
        if(!resolution.error.empty()) {
            LOG_WARN("{}", resolution.error);
        }
        params.tidy = std::move(resolution.params);
    }

    auto unit = clice::compile(std::move(params));
    if(!unit->completed()) {
        entry.error = "compile_error";
        auto& messages = entry.diagnostics.emplace();
        for(const auto& diagnostic: unit->diagnostics()) {
            messages.push_back(diagnostic.message);
            for(const auto& note: diagnostic.notes) {
                messages.push_back(note.message);
            }
        }
        return;
    }

    // The AST builds even for broken sources (a language server must keep
    // working on them), so error diagnostics are reported separately: the
    // test harness rejects code that silently broke instead of pinning
    // garbage.
    if(auto errors = collect_errors(*unit); !errors.empty()) {
        entry.diagnostics = std::move(errors);
    }

    const auto& [points, ranges] = file.markers;
    if(spec.run_complete != nullptr) {
        // Completion shape: the plain compile above only serves the
        // clean-code gate — the completion entry points discard their
        // unit, so its diagnostics are the sole health signal. Each point
        // then compiles again with the offset applied.
        if(points.empty()) {
            entry.error = "no_markers";
            return;
        }
        std::map<std::string, kota::codec::RawValue> markers;
        for(const auto& [name, offset]: points) {
            auto value = spec.run_complete(
                {
                    .kind = CompilationKind::Completion,
                    .input = input,
                    .completion = {.offset = offset},
                },
                config);
            if(!value.has_value()) {
                entry.error = "serialize_error";
                return;
            }
            markers.emplace(name, std::move(*value));
        }
        entry.markers = std::move(markers);
        return;
    }

    if(spec.run_compiles != nullptr) {
        entry.result = spec.run_compiles(input, file.path, config);
        if(!entry.result.has_value()) {
            entry.error = "serialize_error";
        }
        return;
    }

    if(spec.run != nullptr) {
        entry.result = spec.run(*unit, config);
        if(!entry.result.has_value()) {
            entry.error = "serialize_error";
        }
        return;
    }

    if(spec.run_at != nullptr) {
        // Position feature: run once per point. Without any point there is
        // nothing to answer — an error, not an empty result.
        if(points.empty()) {
            entry.error = "no_markers";
            return;
        }
        std::map<std::string, kota::codec::RawValue> markers;
        for(const auto& [name, offset]: points) {
            auto value = spec.run_at(*unit, offset, config);
            if(!value.has_value()) {
                entry.error = "serialize_error";
                return;
            }
            markers.emplace(name, std::move(*value));
        }
        entry.markers = std::move(markers);
        return;
    }

    // Range and selection features run once per range; a selection feature
    // also once per point, as an empty selection, and has no whole-document
    // default.
    std::vector<std::pair<std::string, LocalSourceRange>> places(ranges.begin(), ranges.end());
    auto run = spec.run_select != nullptr ? spec.run_select : spec.run_over;
    if(spec.run_select != nullptr) {
        for(const auto& [name, offset]: points) {
            places.emplace_back(name, LocalSourceRange(offset, offset));
        }
        if(places.empty()) {
            entry.error = "no_markers";
            return;
        }
    }
    if(places.empty()) {
        entry.result = run(*unit,
                           LocalSourceRange(0, static_cast<std::uint32_t>(file.content.size())),
                           config);
        if(!entry.result.has_value()) {
            entry.error = "serialize_error";
        }
        return;
    }
    std::map<std::string, kota::codec::RawValue> markers;
    for(const auto& [name, range]: places) {
        auto value = run(*unit, range, config);
        if(!value.has_value()) {
            entry.error = "serialize_error";
            return;
        }
        markers.emplace(name, std::move(*value));
    }
    entry.markers = std::move(markers);
}

int run_inspect(const InspectOptions& opts, const FeatureSpec& spec) {
    auto args = positional(opts.path.as_optional(), {"<PATH>"});
    if(!args) {
        return usage_error(Errors::Text, args.error());
    }
    Spelling abs_path(args->front(), Spelling::cwd());
    if(!vfs::exists(abs_path)) {
        LOG_ERROR("no such file or directory: {}", abs_path);
        return 1;
    }
    if(vfs::is_directory(abs_path)) {
        LOG_ERROR("{} is a directory, not a source file", abs_path);
        return 1;
    }

    SourceFile file{.path = abs_path.str()};
    if(opts.markers.has_value()) {
        auto buffer = vfs::read(*opts.markers);
        if(!buffer) {
            LOG_ERROR("cannot read --markers {}: {}", *opts.markers, buffer.error().message());
            return 1;
        }
        auto result =
            kota::codec::json::from_string<StrictJson>((*buffer)->getBuffer(), file.markers);
        if(!result) {
            LOG_ERROR("invalid --markers: {}", result.error().message);
            return 1;
        }
        if(file.markers.points.contains("") || file.markers.ranges.contains("")) {
            LOG_ERROR("--markers gives every marker a name");
            return 1;
        }
        // A selection feature answers points and ranges under one name each.
        for(const auto& name: file.markers.points | std::views::keys) {
            if(file.markers.ranges.contains(name)) {
                LOG_ERROR("--markers names a point and a range {}", name);
                return 1;
            }
        }
    }

    std::vector<std::string> flags;
    if(opts.flags.has_value()) {
        if(auto result = kota::codec::json::from_string(*opts.flags, flags); !result) {
            LOG_ERROR("--flags is not a JSON string array: {}", result.error().message);
            return 1;
        }
        if(flags.empty()) {
            LOG_ERROR("--flags must name at least one compile flag");
            return 1;
        }
    }

    // The file belongs to the nearest project at or above it: its
    // configuration, the databases it names (or the one discovered under
    // it) and the dependency graph give the file the command the server
    // would use — the same loading path as `clice serve`.
    auto directory = abs_path.parent();
    FileTable file_table;
    Project project{file_table};
    CommandResolver commands(project);
    if(!flags.empty() && opts.configuration.configuration) {
        LOG_ERROR("--configuration selects among the workspace's rules; --flags replaces them");
        return 1;
    }
    auto overlay = [&] {
        if(!opts.config_overlay) {
            return true;
        }
        auto result =
            kota::codec::json::from_string<StrictJson>(*opts.config_overlay, project.config);
        if(!result) {
            LOG_ERROR("invalid --config-overlay: {}", result.error().message);
        }
        return result.has_value();
    };
    if(flags.empty()) {
        auto identity = CanonicalPath(directory);
        auto root = workspace_of(identity);
        // The overlay goes in before the rules compile, as initializationOptions do.
        project.config = Config::load_from_workspace(root, nullptr, nullptr, /*finalized=*/false);
        if(!overlay()) {
            return 1;
        }
        project.config.finalize(root);
        project.config.keep_own_cache_dir();
        auto requested = opts.configuration.tag();
        if(auto checked = check_requested_configuration(project.config, requested); !checked) {
            LOG_ERROR("{}", checked.error());
            return 1;
        }
        // What the server discovers when the file is opened: the databases
        // between its directory and the root.
        load_build(project,
                   root,
                   resolve_configuration(project.config, requested),
                   compile_commands_above(identity, root));
    } else if(!overlay()) {
        return 1;
    }

    auto name = path::filename(file.path).str();
    auto buffer = vfs::read(file.path);
    if(!buffer) {
        LOG_ERROR("cannot read {}: {}", abs_path, buffer.error().message());
        return 1;
    }
    file.content = (*buffer)->getBuffer().str();
    auto inside = [&](std::uint32_t offset) {
        return offset <= file.content.size();
    };
    if(!std::ranges::all_of(file.markers.points | std::views::values, inside) ||
       !std::ranges::all_of(file.markers.ranges | std::views::values,
                            [&](const LocalSourceRange& range) {
                                return range.begin <= range.end && inside(range.end);
                            })) {
        LOG_ERROR("--markers puts a marker outside the text of {}", name);
        return 1;
    }

    InspectOutput output;
    output.feature = spec.name.str();
    auto& entry = output.files[name];
    auto command = file_command(entry, file.path, flags, directory, file_table, commands);
    if(command) {
        command->workspace = project.config.workspace_root.str();
        run_feature(entry, spec, file, *command, project.config);
    }

    auto json = kota::codec::json::to_string<InspectJsonConfig>(output);
    if(!json) {
        LOG_ERROR("serialization failed: {}", json.error().message);
        return 1;
    }
    driver::println("{}", *json);
    return 0;
}

CommandHandler build(const char*) {
    std::vector<Operation> operations;
    for(const auto& spec: features) {
        auto usage = std::format("clice inspect {} <PATH> [OPTIONS]", spec.name);
        operations.push_back({
            .name = spec.name,
            .description = spec.description,
            .run = spec.check
                       ? operation<CheckedFeatureOptions>(
                             usage,
                             "inspect",
                             Errors::Text,
                             [&spec](const CheckedFeatureOptions& opts) {
                                 return run_inspect(opts.inspect, opts.check ? *spec.check : spec);
                             })
                       : operation<FeatureOptions>(usage,
                                                   "inspect",
                                                   Errors::Text,
                                                   [&spec](const FeatureOptions& opts) {
                                                       return run_inspect(opts.inspect, spec);
                                                   }),
        });
    }
    operations.push_back({
        .name = "config-schema",
        .description = "The JSON schema of the clice configuration",
        .run = operation<SchemaOptions>("clice inspect config-schema [OPTIONS]",
                                        "inspect",
                                        Errors::Text,
                                        [](const SchemaOptions&) {
                                            auto schema = Config::json_schema();
                                            if(!schema) {
                                                LOG_ERROR("config schema generation failed: {}",
                                                          schema.error());
                                                return 1;
                                            }
                                            driver::println("{}", *schema);
                                            return 0;
                                        }),
    });
    return with_operations("Run a feature on a source file and print raw results as JSON",
                           std::move(operations),
                           Errors::Text);
}

[[maybe_unused]] const bool registered =
    register_command(Command::inspect,
                     {.description = "Run a feature on a source file and print raw results as JSON",
                      .build = build});

}  // namespace

}  // namespace clice::driver
