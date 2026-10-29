module clice;

import :command.file_kind;
import :command.search_config;
import :feature.code_actions;
import :feature.code_completion.item;
import :feature.code_completion;
import :feature.protocol;
import :index.query;
import :index.search;
import :index.symbol;
import :semantic.symbol;
import :semantic.token_class;
import :support.fuzzy_matcher;
import :syntax.completion;
import :syntax.dependency_graph;
import :syntax.include_resolver;
import :syntax.scan;
import :vfs.dir_cache;
import :vfs.file_table;
import :vfs.path;

namespace clice::feature {

// What the server completes without a compile, and how it finishes the
// list a compile returns.

auto unqualified_prefix(llvm::StringRef text, std::uint32_t offset)
    -> std::optional<llvm::StringRef> {
    auto prefix = Prefix::from(text, offset);
    if(prefix.spelling.empty() || clang::isDigit(prefix.spelling.front())) {
        return std::nullopt;
    }
    auto before = text.take_front(prefix.typed.begin).rtrim();
    if(before.ends_with("::") || before.ends_with(".") || before.ends_with("->")) {
        return std::nullopt;
    }
    return prefix.spelling;
}

auto completion_list(std::vector<protocol::CompletionItem> items, std::uint32_t limit)
    -> protocol::CompletionList {
    protocol::CompletionList list{.is_incomplete = false, .items = std::move(items)};
    if(limit != 0 && list.items.size() > limit) {
        ranges::sort(list.items, {}, [](const protocol::CompletionItem& item) {
            return std::tie(static_cast<const std::optional<std::string>&>(item.sort_text),
                            item.label);
        });
        list.items.resize(limit);
        list.is_incomplete = true;
    }
    return list;
}

bool scope_candidates(const index::IndexQuery& query,
                      const DependencyGraph& graph,
                      llvm::StringRef prefix,
                      Fid file,
                      std::vector<ScopeCandidate>& candidates) {
    auto kind_of = [](SymbolKind kind) {
        using Kind = protocol::CompletionItemKind;
        switch(kind) {
            case SymbolKind::Namespace: return Kind::Module;
            case SymbolKind::Class:
            case SymbolKind::Struct:
            case SymbolKind::Union: return Kind::Class;
            case SymbolKind::Enum: return Kind::Enum;
            case SymbolKind::EnumMember: return Kind::EnumMember;
            case SymbolKind::Function: return Kind::Function;
            case SymbolKind::Variable: return Kind::Variable;
            case SymbolKind::Type:
            case SymbolKind::Concept: return Kind::TypeParameter;
            default: return Kind::Text;
        }
    };
    auto found = index::completables(query, prefix, file, scope_candidate_limit);
    for(auto& [symbol, declared]: found.names) {
        bool macro = symbol.kind == SymbolKind::Macro;
        ScopeCandidate candidate{
            .name = symbol.name,
            .kind = !macro ? kind_of(symbol.kind)
                    : symbol.token_class.has(SemanticTokenModifier::Callable)
                        ? protocol::CompletionItemKind::Function
                        : protocol::CompletionItemKind::Constant,
            .macro = macro,
            .is_template = index::has_flag(symbol.flags, index::SymbolFlags::Template),
            .deprecated = index::has_flag(symbol.flags, index::SymbolFlags::Deprecated),
            .symbol = symbol.hash,
        };
        // An enumerator keeps its enumeration in the qualifier: the index
        // does not tell a scoped one, which needs it, and an unscoped one
        // takes it as well.
        auto chain = query.container_chain(symbol.hash, declared);
        auto namespaces = llvm::ArrayRef(chain);
        if(!namespaces.empty() && namespaces.back().kind == SymbolKind::Enum) {
            namespaces = namespaces.drop_back();
        }
        if(ranges::any_of(namespaces, [](const index::SymbolInfo& container) {
               return container.kind != SymbolKind::Namespace;
           })) {
            continue;
        }
        for(auto& container: chain) {
            candidate.scope.push_back(container.name);
        }
        if(declared.valid() && index::has_flag(symbol.flags, index::SymbolFlags::Exported)) {
            candidate.module = graph.module_of(declared).str();
        }
        candidates.push_back(std::move(candidate));
    }
    return found.exhausted;
}

/// The header, quotes or brackets included, an `#include` for `candidate`
/// names in `file`, as `search` finds it: a standard name's from the
/// standard library's own table (the C library's for a C file, `c`),
/// another's the header the index says declares it. None when only a
/// library's internal header is known.
static std::optional<std::string> include_for(const index::IndexQuery& query,
                                              const ScopeCandidate& candidate,
                                              bool c,
                                              Fid file,
                                              const SearchConfig& search,
                                              vfs::Scope& scope) {
    auto& files = query.file_table();
    namespace stdlib = clang::tooling::stdlib;
    std::string qualifier;
    for(const auto& segment: candidate.scope) {
        qualifier += segment + "::";
    }
    if(auto symbol = stdlib::Symbol::named(qualifier,
                                           candidate.name,
                                           c ? stdlib::Lang::C : stdlib::Lang::CXX)) {
        if(auto header = symbol->header()) {
            return header->name().str();
        }
    }
    // The canonical declaration's header first, a definition when there is
    // one; then the headers declaring the symbol, for a definition in a
    // source file or in a header no `#include` can name.
    llvm::SmallVector<Fid, 2> headers;
    if(auto located = query.resolve(candidate.symbol, file);
       located && is_header_path(located->site.path)) {
        headers.push_back(located->site.file);
    }
    for(const auto& site: query.sites(candidate.symbol, file, RelationKind::Declaration)) {
        if(site.file.valid() && is_header_path(site.path) &&
           !ranges::contains(headers, site.file)) {
            headers.push_back(site.file);
        }
    }
    auto includer = files.spelling(file);
    for(auto header: headers) {
        if(header == file || is_context_header_path(files.resolve(header))) {
            continue;
        }
        if(auto argument = include_argument(files.resolve(header), search, includer, scope)) {
            return argument;
        }
    }
    return std::nullopt;
}

auto finish_completion(protocol::CompletionList list,
                       const IncludePlace& include,
                       llvm::ArrayRef<ScopeCandidate> candidates,
                       const index::IndexQuery& query,
                       Fid file,
                       const SearchConfig& search,
                       vfs::Scope& scope,
                       std::uint32_t limit) -> protocol::CompletionList {
    IncludeInsertion insertion{.opens_fragment = include.opens_fragment};
    std::erase_if(list.items, [&](protocol::CompletionItem& item) {
        auto symbol = data_symbol(item.data);
        if(!symbol) {
            return false;
        }
        item.data.reset();
        auto candidate = ranges::find_if(candidates, [&](const ScopeCandidate& each) {
            return each.symbol == *symbol;
        });
        assert(candidate != candidates.end() && "the worker names a candidate it was given");
        auto header = include_for(query, *candidate, include.c, file, search, scope);
        if(!header) {
            return true;
        }
        protocol::TextEdit edit{
            .range = {include.position, include.position},
            .new_text = insertion.text(*header),
        };
        item.additional_text_edits = std::vector{std::move(edit)};
        return false;
    });
    auto limited = completion_list(std::move(list.items), limit);
    limited.is_incomplete = limited.is_incomplete || list.is_incomplete;
    return limited;
}

auto include_completion(const CompletionName& completing,
                        llvm::StringRef text,
                        const PositionMap& map,
                        PositionEncoding encoding,
                        llvm::StringRef path,
                        const SearchConfig& search,
                        vfs::Scope& scope,
                        std::uint32_t limit) -> protocol::CompletionList {
    bool angled = (completing.kind == CompletionName::Kind::IncludeAngled);
    auto candidates =
        complete_include_path(search, path::parent(path), completing.prefix, angled, scope);

    // A directory leaves the path open: the `/` typed after it asks
    // again, for the directory's own entries. A header ends the path
    // and closes the directive, replacing the rest of the path through
    // a closing delimiter already there.
    char closer = angled ? '>' : '"';
    auto edit = [&](const IncludeCandidate& candidate) {
        auto end = completing.replace.end;
        if(!candidate.is_directory && completing.closed) {
            end = static_cast<std::uint32_t>(text.find(closer, end)) + 1;
        }
        return protocol::TextEdit{
            .range = *map.range({completing.replace.begin, end}, encoding),
            .new_text = candidate.is_directory ? candidate.name : candidate.name + closer,
        };
    };
    std::vector<protocol::CompletionItem> items;
    items.reserve(candidates.size());
    for(auto& c: candidates) {
        protocol::CompletionItem item;
        item.label = c.is_directory ? c.name + "/" : c.name;
        item.kind = c.is_directory ? protocol::CompletionItemKind::Folder
                                   : protocol::CompletionItemKind::File;
        item.sort_text = std::format("{:010}", c.order);
        item.text_edit = edit(c);
        items.push_back(std::move(item));
    }
    return completion_list(std::move(items), limit);
}

auto directive_completion(const CompletionName& completing,
                          const PositionMap& map,
                          PositionEncoding encoding,
                          const clang::LangOptions& dialect,
                          const CodeCompletionOptions& options,
                          const CompletionClient& client) -> protocol::CompletionList {
    // Clang's CodeCompletePreprocessorDirective, which a compile would
    // answer with, placeholders in braces.
    struct Pattern {
        llvm::StringLiteral name;
        llvm::StringLiteral arguments;
    };

    llvm::SmallVector<Pattern, 32> patterns = {
        {"if",     " {condition}"},
        {"ifdef",  " {macro}"    },
        {"ifndef", " {macro}"    },
    };
    if(completing.conditional == CompletionName::Conditional::Open) {
        patterns.append({
            {"elif",     " {condition}"},
            {"elifdef",  " {macro}"    },
            {"elifndef", " {macro}"    },
            {"else",     ""            },
        });
    }
    if(completing.conditional != CompletionName::Conditional::None) {
        patterns.push_back({"endif", ""});
    }
    patterns.append({
        {"include", R"( "{header}")"           },
        {"include", " <{header}>"              },
        {"define",  " {macro}"                 },
        {"define",  " {macro}({args})"         },
        {"undef",   " {macro}"                 },
        {"line",    " {number}"                },
        {"line",    R"( {number} "{filename}")"},
        {"error",   " {message}"               },
        {"pragma",  " {arguments}"             },
    });
    if(dialect.ObjC) {
        patterns.append({
            {"import", R"( "{header}")"},
            {"import", " <{header}>"   },
        });
    }
    patterns.append({
        {"include_next", R"( "{header}")"},
        {"include_next", " <{header}>"   },
        {"warning",      " {message}"    },
    });
    if(dialect.C23) {
        patterns.append({
            {"embed", R"( "{file}")"},
            {"embed", " <{file}>"   },
        });
    }

    LocalSourceRange typed(completing.replace.begin,
                           completing.replace.begin +
                               static_cast<std::uint32_t>(completing.prefix.size()));
    auto typed_range = map.range(typed, encoding);
    auto whole_range = map.range(completing.replace, encoding);
    bool snippets = options.statement_snippets && client.snippets;
    FuzzyMatcher matcher(completing.prefix);
    llvm::StringSet<> listed;
    std::vector<protocol::CompletionItem> items;
    for(auto& pattern: patterns) {
        auto score = matcher.match(pattern.name);
        // Without its placeholders a pattern is its name: the variants of
        // one directive collapse.
        if(!score || (!snippets && !listed.insert(pattern.name).second)) {
            continue;
        }
        protocol::CompletionItem item{.label = pattern.name.str()};
        item.kind = snippets ? protocol::CompletionItemKind::Snippet
                             : protocol::CompletionItemKind::Keyword;
        std::string text = pattern.name.str();
        if(snippets && !pattern.arguments.empty()) {
            std::string shown;
            unsigned placeholders = 0;
            for(auto rest = llvm::StringRef(pattern.arguments); !rest.empty();) {
                auto [literal, placeholder] = rest.split('{');
                auto [name, after] = placeholder.split('}');
                text += escape_snippet(literal);
                shown += literal;
                if(!name.empty()) {
                    placeholders += 1;
                    text += std::format("${{{}:{}}}", placeholders, name);
                    shown += name;
                }
                rest = after;
            }
            item.insert_text_format = protocol::InsertTextFormat::Snippet;
            item.label_details = protocol::CompletionItemLabelDetails{.detail = std::move(shown)};
        }
        if(client.insert_replace && typed != completing.replace) {
            item.text_edit = protocol::InsertReplaceEdit{
                .new_text = std::move(text),
                .insert = *typed_range,
                .replace = *whole_range,
            };
        } else {
            item.text_edit = protocol::TextEdit{.range = *whole_range, .new_text = std::move(text)};
        }
        item.sort_text = sort_text(*score * priority_weight(clang::CCP_CodePattern), std::nullopt);
        items.push_back(std::move(item));
    }
    return completion_list(std::move(items), options.limit.value);
}

auto module_completion(const CompletionName& completing,
                       llvm::StringRef text,
                       const PositionMap& map,
                       PositionEncoding encoding,
                       llvm::StringRef path,
                       const DependencyGraph& graph,
                       FileTable& files,
                       std::uint32_t limit) -> protocol::CompletionList {
    std::vector<std::string> module_names;
    if(completing.kind == CompletionName::Kind::ModuleDeclaration) {
        module_names = complete_module_declaration(graph, completing.prefix);
    } else {
        // The module declaration precedes every import, and the
        // scan stops before the import being typed: the scanner
        // rejects a buffer with an unclosed directive whole. Its
        // lexer needs the NUL terminator a copy provides.
        auto declared = text.take_front(text.rfind('\n', completing.replace.begin) + 1).str();
        // FIXME: a module declaration inside `#if` scans as none, so the unit's own
        // module is offered and its partitions are not; an import typed in the global
        // module fragment, where none may go, is offered names too.
        module_names = complete_module_import(graph, completing.prefix, scan_quick(declared));
    }
    auto directory = path::parent(path);
    auto up_to = [&](Fid provider) {
        std::uint32_t up = 0;
        auto interface = llvm::StringRef(files.resolve(provider));
        for(auto dir: path::ancestors(directory)) {
            up += 1;
            if(path::is_within(interface, dir)) {
                break;
            }
        }
        return up;
    };
    auto distance = [&](llvm::StringRef name) {
        if(name.starts_with(":")) {
            return std::uint32_t(0);
        }
        return ranges::min(graph.lookup_module(name) | views::transform(up_to));
    };
    std::vector<protocol::CompletionItem> items;
    items.reserve(module_names.size());
    for(auto& name: module_names) {
        protocol::CompletionItem item;
        item.label = name;
        item.kind = protocol::CompletionItemKind::Module;
        item.sort_text = std::format("{:05}", distance(name));
        item.text_edit = protocol::TextEdit{
            .range = *map.range(completing.replace, encoding),
            .new_text = completing.closed ? name : name + ";",
        };
        items.push_back(std::move(item));
    }
    return completion_list(std::move(items), limit);
}

auto start_completion(const CompletionInputs& inputs) -> CompletionStart {
    auto& text = inputs.text;
    auto offset = inputs.positions.offset(inputs.position, inputs.encoding);
    CompletionName completing;
    if(offset) {
        completing = completion_name(text, *offset, inputs.dialect);
    }

    // Anything else a trigger character is typed in — `template<`, a
    // closing `>`, the dots of a pack ellipsis — is answered before any
    // include scanning or completion build.
    if(!inputs.trigger_character.empty()) {
        bool served = inputs.trigger_character == " "
                          ? completing.kind == CompletionName::Kind::Import ||
                                completing.kind == CompletionName::Kind::ModuleDeclaration
                          : completing.kind != CompletionName::Kind::None || !offset ||
                                follows_access_operator(text, *offset, inputs.dialect);
        if(!served) {
            return NoCompletion{};
        }
    }

    auto limit = inputs.options.limit;
    if(offset) {
        switch(completing.kind) {
            case CompletionName::Kind::Directive:
                return directive_completion(completing,
                                            inputs.positions,
                                            inputs.encoding,
                                            inputs.dialect,
                                            inputs.options,
                                            inputs.client);
            case CompletionName::Kind::IncludeQuoted:
            case CompletionName::Kind::IncludeAngled:
                return include_completion(completing,
                                          text,
                                          inputs.positions,
                                          inputs.encoding,
                                          inputs.path,
                                          inputs.search(),
                                          inputs.scope,
                                          limit);
            case CompletionName::Kind::Import:
            case CompletionName::Kind::ModuleDeclaration:
                return module_completion(completing,
                                         text,
                                         inputs.positions,
                                         inputs.encoding,
                                         inputs.path,
                                         inputs.graph,
                                         inputs.files,
                                         limit);
            default: break;
        }
    }

    CompileCompletion compile;
    if(auto prefix = offset ? unqualified_prefix(text, *offset) : std::nullopt;
       prefix && inputs.options.all_scopes && completing.directive.empty()) {
        compile.more_candidates =
            !scope_candidates(inputs.query, inputs.graph, *prefix, inputs.file, compile.candidates);
    }
    return compile;
}

}  // namespace clice::feature
