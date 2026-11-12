module clice;

import :command.file_kind;
import :command.table;
import :compile.compilation;
import :compile.tidy;
import :driver.driver;
import :driver.run;
import :feature.code_actions;
import :feature.code_completion;
import :feature.diagnostics;
import :feature.document_links;
import :feature.document_symbols;
import :feature.folding_ranges;
import :feature.hover;
import :feature.inlay_hints;
import :feature.navigation;
import :feature.selection_ranges;
import :feature.semantic_tokens;
import :feature.signature_help;
import :index.query.entries;
import :index.indexing;
import :index.shard.instantiations;
import :index.project_index;
import :index.query;
import :index.shard;
import :index.indexing.tu_index;
import :project.project;
import :semantic.selection;
import :semantic.semantics;
import :semantic.symbol;
import :semantic.template_resolver;
import :settings.settings;
import :support.text;
import :syntax.source_text;
import :syntax.token;
import :vfs.file_system;
import :vfs.file_table;
import :vfs.path;

namespace clice::driver {

namespace {

/// JSON layout of the inspect output. Field names stay snake_case (the
/// project's native spelling) and enums serialize as their C++ value
/// names; the TS side owns any mapping to LSP vocabulary.
struct JsonFormat : codec::extend<json::format> {};

}  // namespace

}  // namespace clice::driver

template <typename E>
    requires std::is_enum_v<E>
struct kotatsu::codec::repr<E, clice::driver::JsonFormat> : kotatsu::codec::enum_names<> {};

namespace clice::driver {

namespace {

/// What every feature takes.
struct Options {
    KANNOTATE(&path,
              cli::positional = true,
              cli::meta_var = "<PATH>",
              description = "The source file")
    std::string path;

    KANNOTATE(&markers,
              description = R"(JSON file of the markers in the inspected source file, at byte )"
                            R"(offsets: {"points": {"name": 12}, "ranges": {"name": )"
                            R"({"begin": 3, "end": 7}}}; results are keyed by these names)")
    std::optional<std::string> markers;

    KANNOTATE(&config_overlay,
              description =
                  "Configuration overlay as a JSON object of clice.toml sections, over "
                  "the project's configuration")
    std::optional<std::string> config_overlay;

    KANNOTATE(&host,
              description =
                  "Compile the header as included by this source file, as the "
                  "editor's context choice does")
    std::optional<std::string> host;

    KANNOTATE(&occurrence,
              description = "With --host, which place its compile enters the header at, from 0")
    std::optional<std::uint32_t> occurrence;

    ProjectOptions where;
};

/// Text that is not UTF-8 is written as U+FFFD, as the server writes it to
/// the client.
constexpr json::encode_options json_options{.invalid_utf8 = codec::invalid_utf8::Replace};

struct FileEntry {
    /// The whole-document feature payload, absent when compilation failed
    /// or the feature is marker-driven.
    std::optional<codec::raw_value> result;

    /// Marker-driven payloads, keyed by the names --markers gives: position
    /// features run once per point, range features once per range. A null
    /// value records a position with no result (e.g. hover on whitespace).
    std::optional<std::map<std::string, codec::raw_value>> markers;

    std::optional<std::string> error;
    std::optional<std::vector<std::string>> diagnostics;
};

/// The inspected file's markers, as --markers gives them.
KANNOTATE_TYPE(struct FileMarkers, presence_all = optional);
struct FileMarkers {
    std::map<std::string, std::uint32_t> points;

    std::map<std::string, LocalSourceRange> ranges;
};

/// `files` holds the inspected file's entry under its file name.
struct Output {
    std::string feature;
    std::map<std::string, FileEntry> files;
};

template <typename T>
std::optional<codec::raw_value> to_raw_json(const T& value) {
    auto json = json::to_string<JsonFormat>(value, json_options);
    if(!json) {
        LOG_ERROR("serialization failed: {}", json.error().message);
        return std::nullopt;
    }
    return codec::raw_value{std::move(*json)};
}

/// Marker payload for hover: the feature-layer reply before the edge —
/// markdown produced by the same rendering code the server uses, with the
/// symbol range still in byte offsets.
struct HoverResult {
    std::optional<LocalSourceRange> range;
    protocol::MarkupKind kind;
    std::string contents;
};

/// What a feature is asked about in the inspected file.
enum class Shape : std::uint8_t {
    /// The whole file.
    File,
    /// Each point --markers gives; without one there is nothing to answer.
    Points,
    /// Each range --markers gives, else the whole file.
    Ranges,
    /// Each range, and each point as an empty one; without either there is
    /// nothing to answer.
    Selections,
};

/// What one run of a feature is given.
struct FeatureParams {
    CompilationUnit& unit;

    /// What `unit` compiled from, for a feature that compiles on its own.
    const CompilationInput& input;

    const Settings& settings;

    /// The whole file, a marker's range, or a point as an empty range.
    LocalSourceRange range;
};

/// A feature: its operation and line of help, what it is asked about, what
/// --index runs instead, for one the index answers too, and what --audit
/// runs instead, for one that audits its work. `run` gives nullopt when its
/// answer does not serialize.
struct FeatureSpec {
    std::string_view name;
    std::string_view description;
    Shape shape = Shape::File;
    std::optional<codec::raw_value> (*run)(const FeatureParams&) = nullptr;
    const FeatureSpec* index = nullptr;
    const FeatureSpec* audit = nullptr;
};

/// The unit's own TU index, standing in for the server's index of a file
/// whose AST is not current, and the focus file's text as that index
/// serves it.
struct UnitIndex {
    std::vector<std::byte> bytes;
    index::TUIndex tu;
    SourceText source;

    explicit UnitIndex(CompilationUnit& unit) :
        bytes(index::build_tu_index(unit)), tu(index::TUIndex::from_bytes(as_chars(bytes))),
        source(unit.focus_file().content(), unit.lang_options()) {}

    auto entries() const -> index::FileEntries {
        return index::extract_entries(tu.shard_of(tu.focus_file()));
    }

    auto resolver() const {
        return [this](index::SymbolHash hash) -> std::optional<index::SymbolInfo> {
            auto identity = tu.find_symbol(hash);
            if(!identity) {
                return std::nullopt;
            }
            return index::SymbolInfo::from(hash, *identity);
        };
    }

    /// The focus file's include directives as the index resolves them
    /// (ProjectIndex::resolved_includes): those of the TU root and of the
    /// focus file entered again.
    auto includes() const -> std::vector<index::ResolvedInclude> {
        std::vector<index::ResolvedInclude> includes;
        for(std::uint32_t i = 0; i < tu.node_count(); i += 1) {
            auto node = tu.node(i);
            if(node.parent == index::no_node || tu.node(node.parent).file == tu.focus_file()) {
                includes.push_back({.line = node.line, .target = tu.path(node.file).str()});
            }
        }
        auto key = [](const index::ResolvedInclude& include) {
            return std::tie(include.line, include.target);
        };
        ranges::sort(includes, {}, key);
        includes.erase(ranges::unique(includes, {}, key).begin(), includes.end());
        return includes;
    }
};

std::optional<codec::raw_value> run_folding_ranges(const FeatureParams& params) {
    return to_raw_json(feature::folding_ranges(params.unit));
}

std::optional<codec::raw_value> run_folding_ranges_from_index(const FeatureParams& params) {
    UnitIndex index(params.unit);
    return to_raw_json(
        feature::folding_ranges(index.source, index.entries().decls, index.resolver()));
}

std::optional<codec::raw_value> run_semantic_tokens(const FeatureParams& params) {
    const auto& options = params.settings.semantic_tokens;
    return to_raw_json(feature::named_tokens(feature::semantic_tokens(params.unit, options),
                                             feature::semantic_token_legend(options)));
}

std::optional<codec::raw_value> run_semantic_tokens_from_index(const FeatureParams& params) {
    const auto& options = params.settings.semantic_tokens;
    UnitIndex index(params.unit);
    return to_raw_json(feature::named_tokens(
        feature::semantic_tokens(index.source, index.entries(), index.resolver(), options),
        feature::semantic_token_legend(options)));
}

std::optional<codec::raw_value> run_document_symbols(const FeatureParams& params) {
    return to_raw_json(feature::document_symbols(params.unit, params.settings.document_symbols));
}

std::optional<codec::raw_value> run_document_symbols_from_index(const FeatureParams& params) {
    UnitIndex index(params.unit);
    return to_raw_json(feature::document_symbols(index.source,
                                                 index.entries(),
                                                 index.resolver(),
                                                 params.settings.document_symbols));
}

std::optional<codec::raw_value> run_document_links(const FeatureParams& params) {
    return to_raw_json(feature::document_links(params.unit));
}

std::optional<codec::raw_value> run_document_links_from_index(const FeatureParams& params) {
    UnitIndex index(params.unit);
    return to_raw_json(feature::document_links(index.source, index.includes()));
}

struct RawLocation {
    std::string path;
    protocol::Range range;
};

struct RawInlayHintPart {
    std::string value;
    /// Where the piece links, for one naming a symbol the index places or
    /// a target in the focus file.
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
/// the server's — a declaration, else the definition, the focus file's shard
/// first as an open buffer's is.
std::optional<codec::raw_value> run_inlay_hints(const FeatureParams& params) {
    auto& unit = params.unit;
    auto hints = feature::inlay_hints(unit, params.range, params.settings.inlay_hints);

    std::vector<std::byte> bytes;
    index::TUIndex tu;
    auto place = [&](index::SymbolHash symbol) -> std::optional<RawLocation> {
        if(bytes.empty()) {
            bytes = index::build_tu_index(unit, {.all_files = true});
            tu = index::TUIndex::from_bytes(as_chars(bytes));
        }
        auto focus = tu.focus_file();
        for(auto kind: {RelationKind::Declaration, RelationKind::Definition}) {
            for(std::uint32_t i = 0; i < tu.path_count(); i += 1) {
                auto path_id = i == 0 ? focus : i - 1;
                const auto& shard = tu.shard_of(path_id);
                std::optional<LocalSourceRange> found;
                shard.relations_of(symbol, kind, [&](const index::Relation& relation) {
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

/// Point payload for the template resolver: what it answers at the node a
/// point selects. A type, or an alias's, resolves (`type`) to a type that
/// may name a class (`record`); an `auto` variable deduces its `type`; a
/// dependent name finds its `declarations`, and a call's callee the
/// functions the call may call.
struct RawResolution {
    std::optional<std::string> type;
    std::optional<RawLocation> record;
    std::vector<RawLocation> declarations;
};

std::optional<codec::raw_value> run_template_resolver(const FeatureParams& params) {
    auto& unit = params.unit;
    auto tree = SelectionTree::create_right(unit, {params.range.begin, params.range.begin});
    const auto* node = tree.common_ancestor();
    if(!node) {
        return codec::raw_value{};
    }
    auto& resolver = unit.get<types::TemplateResolver>();
    // Another file's positions are read off the file, as the server reads
    // those of a note.
    llvm::StringMap<std::optional<SourceText>> texts;
    auto place = [&](const clang::Decl* decl) -> std::optional<RawLocation> {
        auto [file, range] = unit.produced_range(clang::SourceRange(decl->getLocation()));
        if(file.isInvalid() || unit.is_builtin_file(file)) {
            return std::nullopt;
        }
        if(unit.focus_file().contains(file)) {
            auto position = unit.focus_file().positions().range(range, PositionEncoding::UTF16);
            if(!position) {
                return std::nullopt;
            }
            return RawLocation{.path = unit.file_path(file).str(), .range = *position};
        }
        auto begin = unit.source_of({file, range.begin});
        auto end = unit.source_of({file, range.end});
        assert(begin && end && "a declaration spelled in text a header context added");
        auto [it, inserted] = texts.try_emplace(begin->path);
        if(inserted) {
            if(auto text = vfs::read_text(begin->path)) {
                it->second.emplace(std::move(*text));
            }
        }
        auto position = it->second ? it->second->positions().range({begin->offset, end->offset},
                                                                   PositionEncoding::UTF16)
                                   : std::nullopt;
        if(!position) {
            return std::nullopt;
        }
        return RawLocation{.path = begin->path.str(), .range = *position};
    };
    RawResolution out;
    auto find = [&](auto&& decls) {
        for(const clang::Decl* decl: decls) {
            if(auto at = place(decl)) {
                out.declarations.push_back(std::move(*at));
            }
        }
    };
    auto resolve = [&](clang::QualType type) {
        if(auto resolved = resolver.resolve(type); !resolved.isNull()) {
            out.type = resolved.getAsString(unit.context().getPrintingPolicy());
        }
        if(const auto* tag = resolver.resolve_tag(type)) {
            out.record = place(tag);
        }
        if(const auto* name = type->getAs<clang::DependentNameType>()) {
            find(resolver.lookup(name));
        }
    };
    if(const auto* loc = node->get<clang::TypeLoc>()) {
        resolve(loc->getType());
    } else if(const auto* alias = node->get<clang::TypedefNameDecl>()) {
        resolve(alias->getUnderlyingType());
    } else if(const auto* var = node->get<clang::VarDecl>()) {
        if(!var->getType()->getContainedAutoType()) {
            resolve(var->getType());
        } else if(auto deduced = resolver.deduce(var); !deduced.isNull()) {
            out.type = deduced.getAsString(unit.context().getPrintingPolicy());
        }
    } else if(const auto* expr = node->get<clang::Expr>()) {
        const auto* outer = node->outer_implicit().parent;
        const auto* call = outer ? outer->get<clang::CallExpr>() : nullptr;
        if(call && call->getCallee()->IgnoreParenImpCasts() == expr) {
            find(resolver.callees(call));
        } else if(const auto* scoped = llvm::dyn_cast<clang::DependentScopeDeclRefExpr>(expr)) {
            find(resolver.lookup(scoped));
        } else if(const auto* member = llvm::dyn_cast<clang::CXXDependentScopeMemberExpr>(expr)) {
            find(resolver.lookup(member));
        }
    }
    return to_raw_json(out);
}

/// One place of the focus file the resolver worked on (run_template_resolver_audit).
struct RawAuditPlace {
    protocol::Range range;

    /// The node's kind, and its text's first line.
    std::string node;

    std::string text;

    std::uint32_t queries = 0;

    std::uint32_t steps = 0;

    /// How often each Trouble struck its queries.
    std::map<std::string, std::uint32_t> troubles;
};

struct RawAudit {
    std::uint32_t queries = 0;

    std::uint32_t steps = 0;

    std::map<std::string, std::uint32_t> troubles;

    /// Costliest first.
    std::vector<RawAuditPlace> places;
};

/// What the resolver costs each place of the file the indexer asks it about,
/// and where its work was cut short or a choice left open.
std::optional<codec::raw_value> run_template_resolver_audit(const FeatureParams& params) {
    auto& unit = params.unit;
    auto focus = unit.focus_file();
    auto semantics = Semantics::build(unit);
    types::TemplateResolver resolver(unit, {.only_instantiation = false, .audit = true});
    auto entries = semantics.node_entries();
    RawAudit out;
    auto add = [](std::map<std::string, std::uint32_t>& counts, const types::QueryAudit& audit) {
        for(auto [kind, count]: views::enumerate(audit.troubles)) {
            if(count != 0) {
                counts[std::string(meta::enum_name(static_cast<types::Trouble>(kind)))] += count;
            }
        }
    };
    for(std::uint32_t i = 0; i < entries.size(); i += 1) {
        auto& node = entries[i].node;
        auto range = node.is_ast() ? focus.produced_range(node.source_range()) : std::nullopt;
        auto position =
            range ? focus.positions().range(*range, PositionEncoding::UTF16) : std::nullopt;
        if(!position) {
            continue;
        }
        auto before = resolver.audits().size();
        resolve_occurrences(semantics, i, &resolver);
        auto audits = resolver.audits().drop_front(before);
        if(audits.empty()) {
            continue;
        }
        RawAuditPlace place{
            .range = *position,
            .node = std::string(meta::enum_name(node.kind())),
            .text = focus.content()
                        .slice(range->begin, range->end)
                        .split('\n')
                        .first.trim()
                        .take_front(80)
                        .str(),
            .queries = static_cast<std::uint32_t>(audits.size()),
        };
        for(auto& audit: audits) {
            place.steps += audit.steps;
            add(place.troubles, audit);
            add(out.troubles, audit);
        }
        out.queries += place.queries;
        out.steps += place.steps;
        if(place.steps != 0 || !place.troubles.empty()) {
            out.places.push_back(std::move(place));
        }
    }
    ranges::stable_sort(out.places, std::greater{}, &RawAuditPlace::steps);
    return to_raw_json(out);
}

std::optional<codec::raw_value> run_selection_range(const FeatureParams& params) {
    return to_raw_json(feature::selection_ranges(params.unit, {params.range.begin}).front());
}

struct RawCodeAction {
    std::string title;
    std::string kind;
    std::vector<TextReplacement> edits;
    /// The definitions a host-source request would place; the request
    /// itself stays unresolved here, having no host and no index.
    std::optional<std::vector<std::string>> host_definitions;
};

/// The actions resolved as an empty index would: definitions into the
/// focus file are all kept, no header declares any name.
std::optional<codec::raw_value> run_code_action(const FeatureParams& params) {
    auto& unit = params.unit;
    std::vector<RawCodeAction> out;
    auto actions = feature::resolve_actions(feature::code_actions(unit, params.range),
                                            {},
                                            {
                                                .path = unit.file_path(unit.focus_file().file()),
                                                .content = unit.focus_file().content(),
                                            });
    for(auto& action: actions) {
        out.push_back({
            .title = std::move(action.title),
            .kind = std::string(action.kind),
            .edits = std::move(action.edits),
        });
        if(!action.host_definitions.empty()) {
            out.back().host_definitions = std::move(action.host_definitions);
        }
    }
    return to_raw_json(out);
}

/// nullopt = serialization failure; an empty raw_value serializes as null
/// and records a marker with no hover. The card is markdown, as a client
/// that shows markdown gets it.
std::optional<codec::raw_value> run_hover(const FeatureParams& params) {
    const auto& settings = params.settings;
    auto info = feature::hover_info(params.unit,
                                    params.range.begin,
                                    settings.hover,
                                    settings.documentation);
    if(!info) {
        return codec::raw_value{};
    }
    return to_raw_json(HoverResult{info->symbol_range,
                                   protocol::MarkupKind::Markdown,
                                   info->present().as_markdown()});
}

/// Code completion and signature help drive their own compilation: the
/// offset parameterizes the parse itself, so each point gets a fresh
/// completion compile instead of a query against the shared unit. That unit
/// only serves the clean-code gate — the completion entry points discard
/// their unit, so its diagnostics are the sole health signal.
CompilationParams completion_at(const FeatureParams& params) {
    return {
        .kind = CompilationKind::Completion,
        .input = params.input,
        .completion = {.offset = params.range.begin},
    };
}

std::optional<codec::raw_value> run_code_completion(const FeatureParams& params) {
    const auto& settings = params.settings;
    // The replies of an editor client that takes everything an item can carry.
    return to_raw_json(
        feature::code_complete(completion_at(params),
                               settings.code_completion,
                               settings.documentation,
                               {.snippets = true, .insert_replace = true, .markdown = true})
            .list);
}

std::optional<codec::raw_value> run_signature_help(const FeatureParams& params) {
    const auto& settings = params.settings;
    return to_raw_json(feature::signature_help(completion_at(params),
                                               settings.signature_help,
                                               settings.documentation));
}

/// Marker payload for document-highlight: the unit's own TU index stands
/// in for the server's file index, the cursor resolved by the same rule.
std::optional<codec::raw_value> run_document_highlight(const FeatureParams& params) {
    auto offset = params.range.begin;
    auto bytes = index::build_tu_index(params.unit);
    auto tu = index::TUIndex::from_bytes(as_chars(bytes));
    const index::Shard& shard = tu.shard_of(tu.focus_file());
    auto named = index::named_at(shard, offset, [&](index::SymbolHash hash) {
        auto symbol = tu.find_symbol(hash);
        return symbol && symbol->kind == SymbolKind::Macro;
    });
    llvm::SmallVector<const index::InstantiationShard*, 1> instantiations;
    if(auto section = tu.instantiation_section_of(tu.focus_file())) {
        instantiations.push_back(&tu.instantiation_shard(*section));
        index::add_resolved(*instantiations.front(), offset, true, named);
    }
    if(!named) {
        return to_raw_json(std::vector<feature::Highlight>{});
    }
    return to_raw_json(
        feature::highlights({&shard},
                            instantiations,
                            named->symbols,
                            [&](index::SymbolHash hash) -> std::optional<SymbolKind> {
                                if(auto symbol = tu.find_symbol(hash)) {
                                    return symbol->kind;
                                }
                                return std::nullopt;
                            }));
}

constexpr FeatureSpec document_links_from_index{.name = "document-links",
                                                .run = run_document_links_from_index};
constexpr FeatureSpec document_symbols_from_index{.name = "document-symbols",
                                                  .run = run_document_symbols_from_index};
constexpr FeatureSpec folding_ranges_from_index{.name = "folding-range",
                                                .run = run_folding_ranges_from_index};
constexpr FeatureSpec semantic_tokens_from_index{.name = "semantic-tokens",
                                                 .run = run_semantic_tokens_from_index};
constexpr FeatureSpec template_resolver_audit{.name = "template-resolver",
                                              .run = run_template_resolver_audit};

constexpr std::array features = {
    FeatureSpec{.name = "code-action",
                .description = "The code actions on a selection",
                .shape = Shape::Selections,
                .run = run_code_action},
    FeatureSpec{.name = "code-completion",
                .description = "The completions at a point",
                .shape = Shape::Points,
                .run = run_code_completion},
    FeatureSpec{.name = "document-highlight",
                .description = "The places naming the symbol at a point",
                .shape = Shape::Points,
                .run = run_document_highlight},
    FeatureSpec{.name = "document-links",
                .description = "The file's links",
                .run = run_document_links,
                .index = &document_links_from_index},
    FeatureSpec{.name = "document-symbols",
                .description = "The file's outline",
                .run = run_document_symbols,
                .index = &document_symbols_from_index},
    FeatureSpec{.name = "folding-range",
                .description = "The file's folding ranges",
                .run = run_folding_ranges,
                .index = &folding_ranges_from_index},
    FeatureSpec{.name = "hover",
                .description = "The hover card at a point",
                .shape = Shape::Points,
                .run = run_hover},
    FeatureSpec{.name = "inlay-hint",
                .description = "The inlay hints over a range, else the file",
                .shape = Shape::Ranges,
                .run = run_inlay_hints},
    FeatureSpec{.name = "selection-range",
                .description = "The selection ranges at a point",
                .shape = Shape::Points,
                .run = run_selection_range},
    FeatureSpec{.name = "semantic-tokens",
                .description = "The file's semantic tokens",
                .run = run_semantic_tokens,
                .index = &semantic_tokens_from_index},
    FeatureSpec{.name = "signature-help",
                .description = "The signature help at a point",
                .shape = Shape::Points,
                .run = run_signature_help},
    FeatureSpec{.name = "template-resolver",
                .description = "What the template resolver answers at a point",
                .shape = Shape::Points,
                .run = run_template_resolver,
                .audit = &template_resolver_audit},
};

/// The inspected file, with the markers --markers places in it.
struct SourceFile {
    std::string path;
    std::string content;
    FileMarkers markers;
};

/// The C-family source at `path`, with the markers the file `markers`
/// places in it.
auto read_source(const Spelling& path, const std::optional<std::string>& markers)
    -> std::expected<SourceFile, std::string> {
    if(!fs::exists(path)) {
        return std::unexpected(std::format("no such file or directory: {}", path));
    }
    if(fs::is_directory(path)) {
        return std::unexpected(std::format("{} is a directory, not a source file", path));
    }
    SourceFile file{.path = path.str()};
    auto name = path::filename(file.path);
    if(is_other_language_path(file.path)) {
        return std::unexpected(std::format("{}: not a C-family source file", name));
    }
    auto content = vfs::read_text(file.path);
    if(!content) {
        return std::unexpected(std::format("{}", content.error()));
    }
    file.content = std::move(*content);
    if(!markers) {
        return file;
    }

    auto text = vfs::read_text(*markers);
    if(!text) {
        return std::unexpected(std::format("--markers: {}", text.error()));
    }
    auto& [points, ranges] = file.markers;
    // An unknown key is a typo, not something to silently ignore.
    if(auto result = json::from_string(*text, file.markers, {.deny_unknown_fields = true});
       !result) {
        return std::unexpected(std::format("invalid --markers: {}", result.error().message));
    }
    if(points.contains("") || ranges.contains("")) {
        return std::unexpected("--markers gives every marker a name");
    }
    // A selection feature answers points and ranges under one name each.
    for(const auto& name: points | views::keys) {
        if(ranges.contains(name)) {
            return std::unexpected(std::format("--markers names a point and a range {}", name));
        }
    }
    auto inside = [&](std::uint32_t offset) {
        return offset <= file.content.size();
    };
    if(!ranges::all_of(points | views::values, inside) ||
       !ranges::all_of(ranges | views::values, [&](const LocalSourceRange& range) {
           return range.begin <= range.end && inside(range.end);
       })) {
        return std::unexpected(std::format("--markers puts a marker outside the text of {}", name));
    }
    return file;
}

/// The project the file in `directory` belongs to, opened as `clice serve`
/// opens it: the project root at or above it (or --workspace), its
/// settings with --config-overlay laid over them as initializationOptions
/// are; its index when one was built, which tells the server's hosts and
/// what their self-containment checks found; the databases the settings
/// name, else the ones discovered under its root and those between the root
/// and the file; the dependency graph.
std::expected<std::unique_ptr<OpenedProject>, ProjectError> load_inspected(const Options& opts,
                                                                           const Spelling& path) {
    LoadSettingsOptions options{.overlay_name = "--config-overlay"};
    if(opts.config_overlay) {
        auto overlay = json::from_string<codec::dyn::Value>(*opts.config_overlay);
        if(!overlay) {
            return std::unexpected(ProjectError{
                .message = std::format("invalid --config-overlay: {}", overlay.error().message),
                .exit_code = 2});
        }
        options.overlay = std::move(*overlay);
    }
    auto read = read_project(opts.where, std::move(options), path.parent());
    if(!read) {
        return std::unexpected(std::move(read.error()));
    }
    auto& [root, loaded] = *read;
    auto opened = open_at(root,
                          std::move(loaded),
                          {
                              .cache = Project::OpenOptions::Cache::Read,
                              .files = {CanonicalPath(path)},
                          });
    if(!opened->opened.unreadable.empty()) {
        LOG_INFO("{}", opened->opened.unreadable);
    }
    return opened;
}

/// clice's build of the precompiled header the command reads, as the server
/// builds it for the editor: under its writer's command the header set holds what the
/// compiler compiled, which the unit's own flags may not (a target reusing
/// another target's PCH). Empty when it does not build, or when it reads
/// `file` itself, whose text its guard would then skip.
std::string build_pch(Project& project,
                      const CommandRef& writer,
                      llvm::StringRef file,
                      llvm::StringRef project_root) {
    std::string dir;
    sys::temp_dir(dir);
    std::string output;
    if(auto created = fs::create_temp_file(output, dir, "clice-inspect-", ".pch"); !created) {
        LOG_WARN("cannot create a temporary PCH: {}", created.error().message());
        return {};
    }
    auto rendered = project.render(writer);
    auto unit = clice::compile({
        .kind = CompilationKind::PrecompiledHeader,
        .input = {.arguments = std::move(rendered.arguments),
                  .directory = std::move(rendered.directory),
                  .project_root = project_root.str(),
                  .vfs = llvm::makeIntrusiveRefCnt<vfs::View>(std::move(rendered.hidden))},
        .output_file = output,
    });
    // The writer's own source is no dep of its unit, but the PCH holds it.
    auto& files = project.file_table;
    auto self = files.intern(Spelling::absolute(file));
    bool reads = writer.file == self || ranges::any_of(unit->deps(), [&](const ReportedInput& dep) {
                     return files.intern(Spelling::absolute(dep.path)) == self;
                 });
    bool built = unit->has_output() && !reads;
    // Destroying the unit writes the PCH to disk.
    unit.reset();
    if(!built) {
        fs::remove(output);
        return {};
    }
    return output;
}

/// What the file compiles with in its project: the resolution the server
/// performs for a background compile, or for the editor under `choice` — the
/// file's entry, a host's command for a header, a rule's default command,
/// the builtin fallback — the include trees the host search needs taken
/// first, and the self-containment check run when none stands for the
/// header, both as the server takes them; with the header context it
/// synthesizes and the precompiled header the build declares, built
/// (build_pch). An error when `choice` names a host whose compile does not
/// enter the file where it says.
std::expected<CompilationInput, std::string> project_input(const SourceFile& file,
                                                           Project& project,
                                                           const ContextChoice* choice) {
    CompilationInput input{.project_root = project.root.str()};
    auto path_id = project.file_table.intern(Spelling::absolute(file.path));
    auto resolve = [&] {
        auto [resolved] = async::run(
            project.resolve_with_trees(path_id, [&] { return CommandRequest{.choice = choice}; }));
        auto& resolution = *resolved;
        input.directory = resolution.directory;
        input.arguments = resolution.arguments;
        return resolution;
    };
    auto resolution = resolve();
    /// The check's compile under the resolution standing: alone, or in the
    /// host's context.
    auto check = [&] {
        auto checked = input;
        checked.vfs = llvm::makeIntrusiveRefCnt<vfs::View>(resolution.hidden);
        checked.add_focus(file.path, file.content);
        if(resolution.synthesized) {
            checked.add_synthesized(resolution.synthesized->files);
        }
        return clice::compile({
            .kind = CompilationKind::Content,
            .input = std::move(checked),
            .skip_bodies = project.settings.project.skip_header_bodies,
        });
    };
    if(auto key = resolution.self_containment; key && !resolution.self_contained) {
        // As the server checks: alone, then in the host's context when that
        // errs, the one with fewer errors standing, the header alone on a tie.
        auto alone = check();
        if(alone->completed()) {
            auto& files = project.file_table;
            auto errors = feature::count_errors(*alone);
            auto text = files.intern_version(path_id, llvm::xxh3_64bits(file.content));
            auto inputs = intern_inputs(files, alone->deps(), alone->started_at().count());
            project.commit_self_containment(*key, errors == 0, text, inputs);
            resolution = resolve();
            if(errors > 0 && resolution.synthesized) {
                auto in_host = check();
                if(in_host->completed()) {
                    // The verdict rests on both compiles, the includer chain
                    // the context embeds among what the host's read.
                    auto read =
                        intern_inputs(files, in_host->deps(), in_host->started_at().count());
                    inputs.append(read.begin(), read.end());
                    inputs.append(resolution.inputs.begin(), resolution.inputs.end());
                    bool self_contained = feature::count_errors(*in_host) >= errors;
                    project.commit_self_containment(*key, self_contained, text, std::move(inputs));
                    if(self_contained) {
                        resolution = resolve();
                    }
                }
            }
        }
    }
    if(choice && (resolution.host != choice->host ||
                  resolution.occurrence != choice->occurrence.value_or(0))) {
        auto host = project.file_table.resolve(choice->host);
        auto times = choice->occurrence.value_or(0) + 1;
        return std::unexpected(
            times == 1 ? std::format("the compile of {} does not enter {}", host, file.path)
                       : std::format("the compile of {} enters {} fewer than {} times",
                                     host,
                                     file.path,
                                     times));
    }
    input.vfs = llvm::makeIntrusiveRefCnt<vfs::View>(std::move(resolution.hidden));
    input.add_focus(file.path, file.content);
    if(resolution.synthesized) {
        input.add_synthesized(resolution.synthesized->files);
    }
    // As the server parses: over the precompiled header the build declares
    // when its build is the compile of the unit writing it.
    if(auto writer = project.pch_writer(path_id, resolution);
       writer && project.declared_pch_request(*writer).writer) {
        if(auto pch = build_pch(project, *writer, file.path, input.project_root); !pch.empty()) {
            input.pch = {std::move(pch), 0};
        }
    }
    return input;
}

/// Run the feature over the file and fill its entry. The compile is one
/// pass, deliberately without the preamble PCH the server uses: a shared
/// snapshot pins that the PCH split does not change feature results, so any
/// divergence between the two paths shows as a snapshot mismatch instead of
/// hiding in the preamble. It skips the function bodies the server's
/// preamble leaves out, and loads the precompiled header the build declares
/// (build_pch). With `diagnostics.clang_tidy` the compile runs the file's
/// clang-tidy configuration as the server's does.
void run_feature(FileEntry& entry,
                 const FeatureSpec& spec,
                 const SourceFile& file,
                 const CompilationInput& input,
                 const Settings& settings) {
    CompilationParams params{
        .kind = CompilationKind::Content,
        .input = input,
        .skip_bodies = settings.project.skip_header_bodies,
    };
    if(settings.diagnostics.clang_tidy) {
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

    auto run = [&](LocalSourceRange range) {
        return spec.run({.unit = *unit, .input = input, .settings = settings, .range = range});
    };
    const auto& [points, ranges] = file.markers;
    std::vector<std::pair<std::string, LocalSourceRange>> places;
    if(spec.shape == Shape::Ranges || spec.shape == Shape::Selections) {
        places.assign(ranges.begin(), ranges.end());
    }
    if(spec.shape == Shape::Points || spec.shape == Shape::Selections) {
        for(const auto& [name, offset]: points) {
            places.emplace_back(name, LocalSourceRange(offset, offset));
        }
    }
    if(spec.shape == Shape::File || (spec.shape == Shape::Ranges && places.empty())) {
        entry.result = run(LocalSourceRange(0, static_cast<std::uint32_t>(file.content.size())));
        if(!entry.result) {
            entry.error = "serialize_error";
        }
        return;
    }
    if(places.empty()) {
        entry.error = "no_markers";
        return;
    }
    std::map<std::string, codec::raw_value> markers;
    for(const auto& [name, range]: places) {
        auto value = run(range);
        if(!value) {
            entry.error = "serialize_error";
            return;
        }
        markers.emplace(name, std::move(*value));
    }
    entry.markers = std::move(markers);
}

int run_inspect(const Options& opts, const FeatureSpec& spec) {
    Spelling path(opts.path, Spelling::cwd());
    auto file = read_source(path, opts.markers);
    if(!file) {
        LOG_ERROR("{}", file.error());
        return 1;
    }
    auto opened = load_inspected(opts, path);
    if(!opened) {
        return print_error(opened.error());
    }
    auto& project = (*opened)->project;
    std::optional<ContextChoice> choice;
    if(opts.host) {
        choice = ContextChoice{
            .host = project.file_table.intern(Spelling(*opts.host, Spelling::cwd())),
            .occurrence = opts.occurrence,
        };
    }

    Output output{.feature = std::string(spec.name)};
    auto& entry = output.files[std::string(path::filename(file->path))];
    auto input = project_input(*file, project, choice ? &*choice : nullptr);
    if(!input) {
        LOG_ERROR("--host: {}", input.error());
        return 1;
    }
    run_feature(entry, spec, *file, *input, project.settings);
    if(!input->pch.first.empty()) {
        fs::remove(input->pch.first);
    }

    auto json = json::to_string<JsonFormat>(output, json_options);
    if(!json) {
        LOG_ERROR("serialization failed: {}", json.error().message);
        return 1;
    }
    driver::println("{}", *json);
    return 0;
}

/// The options of features[I].
template <std::size_t I>
struct Feature {
    Options inspect;
};

/// The options of features[I], which the index answers too.
template <std::size_t I>
struct IndexedFeature {
    Options inspect;

    KANNOTATE(&index,
              description =
                  "Answer from the file's index, as the server does while the file's "
                  "AST is not current")
    bool index = false;
};

/// The options of features[I], which audits its work.
template <std::size_t I>
struct AuditedFeature {
    Options inspect;

    KANNOTATE(&audit,
              description =
                  "Where the work goes in the whole file, and where it gives up or "
                  "leaves a choice open")
    bool audit = false;
};

template <std::size_t I>
consteval auto __kotatsu__type_annotation(std::type_identity<Feature<I>>) {
    return meta::make_annotation(codec::dsl::description = features[I].description);
}

template <std::size_t I>
consteval auto __kotatsu__type_annotation(std::type_identity<IndexedFeature<I>>) {
    return meta::make_annotation(codec::dsl::description = features[I].description);
}

template <std::size_t I>
consteval auto __kotatsu__type_annotation(std::type_identity<AuditedFeature<I>>) {
    return meta::make_annotation(codec::dsl::description = features[I].description);
}

template <std::size_t I>
int run(Feature<I>& feature) {
    return run_inspect(feature.inspect, features[I]);
}

template <std::size_t I>
int run(IndexedFeature<I>& feature) {
    return run_inspect(feature.inspect, feature.index ? *features[I].index : features[I]);
}

template <std::size_t I>
int run(AuditedFeature<I>& feature) {
    return run_inspect(feature.inspect, feature.audit ? *features[I].audit : features[I]);
}

KANNOTATE_TYPE(struct ConfigSchema, description = "The JSON schema of the clice configuration");
struct ConfigSchema {};

int run(ConfigSchema&) {
    auto schema = Settings::json_schema();
    if(!schema) {
        LOG_ERROR("config schema generation failed: {}", schema.error());
        return 1;
    }
    driver::println("{}", *schema);
    return 0;
}

/// The operations of `clice inspect`: each feature, and the schema.
struct Operations {
    using signature = int();
};

KANNOTATE_TYPE(struct Inspect,
               description = "Run a feature on a source file and print raw results as JSON");
struct Inspect {
    KANNOTATE(&operation, presence = required, cli::meta_var = "<feature>")
    cli::Commands<Operations> operation;
};

[[maybe_unused]] const bool added = [] {
    []<std::size_t... I>(std::index_sequence<I...>) {
        (cli::add_command<Operations>(
             dispatch<std::conditional_t<
                 features[I].index != nullptr,
                 IndexedFeature<I>,
                 std::conditional_t<features[I].audit != nullptr, AuditedFeature<I>, Feature<I>>>>,
             {.name = features[I].name}),
         ...);
    }(std::make_index_sequence<features.size()>{});
    cli::add_command<Operations>(dispatch<ConfigSchema>);
    return cli::add_command<Subcommands>(dispatch<Inspect>);
}();

}  // namespace

}  // namespace clice::driver
