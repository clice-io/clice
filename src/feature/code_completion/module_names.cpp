module clice;

import :feature.code_completion.cursor;
import :feature.code_completion.module_names;
import :feature.code_completion.ranking;
import :feature.code_completion;
import :feature.documentation;
import :index.query;
import :index.site;
import :semantic.identity;
import :semantic.symbol;
import :support.fuzzy_matcher;
import :syntax.dependency_graph;
import :syntax.scan;

namespace clice::feature {

namespace {

/// The modules the project provides and the units providing them: the
/// graph's, an open unit's as its buffer declares it (`open`).
struct ModuleProviders {
    llvm::StringMap<llvm::SmallVector<Fid, 2>> modules;

    /// The providers of internal partitions.
    llvm::DenseSet<Fid> internal;
};

auto module_providers(const DependencyGraph& graph, llvm::ArrayRef<OpenModule> open)
    -> ModuleProviders {
    ModuleProviders providers;
    for(auto& entry: graph.modules()) {
        for(auto file: entry.getValue()) {
            if(ranges::contains(open, file, &OpenModule::file)) {
                continue;
            }
            providers.modules[entry.getKey()].push_back(file);
            if(graph.internal_partition(file)) {
                providers.internal.insert(file);
            }
        }
    }
    for(const auto& each: open) {
        if(each.name.empty()) {
            continue;
        }
        providers.modules[each.name].push_back(each.file);
        if(each.internal) {
            providers.internal.insert(each.file);
        }
    }
    return providers;
}

/// Matches a module name being typed: up to its last `.`, as written; the
/// rest by the words of a name, as an include path matches its directories
/// and then its file name.
class NameMatcher {
public:
    explicit NameMatcher(llvm::StringRef typed) :
        scope(typed.take_front(typed.contains('.') ? typed.rfind('.') + 1 : 0)),
        matcher(typed.drop_front(scope.size())) {}

    std::optional<float> match(llvm::StringRef name) {
        if(!name.starts_with(scope)) {
            return std::nullopt;
        }
        return matcher.match(name.drop_front(scope.size()));
    }

private:
    llvm::StringRef scope;

    FuzzyMatcher matcher;
};

/// A module name a completion offers, with how well the name typed
/// matches it (FuzzyMatcher's score).
struct ModuleCandidate {
    std::string name;
    float score = 0;
};

/// The names an implementation unit's module declaration can take that
/// `typed` matches: the modules the interfaces provide.
std::vector<ModuleCandidate> complete_module_declaration(const ModuleProviders& providers,
                                                         llvm::StringRef typed) {
    NameMatcher matcher(typed);
    std::vector<ModuleCandidate> results;
    for(auto& entry: providers.modules) {
        auto name = entry.getKey();
        if(name.contains(':')) {
            continue;
        }
        if(auto score = matcher.match(name)) {
            results.push_back({.name = name.str(), .score = *score});
        }
    }
    return results;
}

/// The names an `import` in the unit whose module declaration `unit`
/// scanned can take that `typed` matches: the other provided modules, and
/// the partitions of the unit's own module but its own, spelled
/// `:partition` — in an interface unit, its interface partitions only.
std::vector<ModuleCandidate> complete_module_import(const ModuleProviders& providers,
                                                    llvm::StringRef typed,
                                                    const ScanResult& unit) {
    auto [module, own_partition] = llvm::StringRef(unit.module_name).split(':');
    NameMatcher matcher(typed);
    std::vector<ModuleCandidate> results;
    for(auto& entry: providers.modules) {
        // A partition is imported by its own module alone, by the partition
        // name; a unit never imports its own module or itself.
        auto [owner, partition] = entry.getKey().split(':');
        std::string name;
        if(partition.empty()) {
            if(owner == module) {
                continue;
            }
            name = owner.str();
        } else {
            if(module.empty() || owner != module || partition == own_partition ||
               (unit.is_interface_unit && providers.internal.contains(entry.getValue().front()))) {
                continue;
            }
            name = std::format(":{}", partition);
        }
        if(auto score = matcher.match(name)) {
            results.push_back({.name = std::move(name), .score = *score});
        }
    }
    return results;
}

}  // namespace

auto module_completion(const CompletionName& completing, const CompletionInputs& inputs)
    -> protocol::CompletionList {
    auto& text = inputs.text;
    auto limit = inputs.options.limit;
    auto open = inputs.open_modules();
    std::vector<ModuleCandidate> candidates;
    ModuleProviders providers;
    std::string own_module;
    if(completing.kind == CompletionName::Kind::ModuleDeclaration) {
        // The declaration being typed provides nothing yet.
        open.push_back({.file = inputs.file});
        providers = module_providers(inputs.graph, open);
        candidates = complete_module_declaration(providers, completing.prefix);
    } else {
        // The module declaration precedes every import, and the
        // scan stops before the import being typed: the scanner
        // rejects a buffer with an unclosed directive whole. Its
        // lexer needs the NUL terminator a copy provides.
        auto declared = text.take_front(text.rfind('\n', completing.replace.begin) + 1).str();
        auto unit = scan_quick(declared);
        if(unit.need_preprocess) {
            // The fragment `module;` opened lasts unless the preprocessor
            // reads a declaration after it.
            bool opened = unit.in_global_fragment;
            unit = inputs.declared_module(declared);
            unit.in_global_fragment = opened && unit.module_name.empty();
        }
        if(unit.in_global_fragment) {
            // No import may go there.
            return completion_list({}, limit);
        }
        open.push_back({
            .file = inputs.file,
            .name = unit.provided_module().str(),
            .internal = unit.is_internal_partition(),
        });
        providers = module_providers(inputs.graph, open);
        candidates = complete_module_import(providers, completing.prefix, unit);
        own_module = llvm::StringRef(unit.module_name).split(':').first.str();
    }
    // The module a name completes to and the unit providing it an import
    // resolves to, the nearest.
    auto module_of = [&](llvm::StringRef name) {
        return name.starts_with(":") ? own_module + name.str() : name.str();
    };
    auto nearest = [&](llvm::StringRef name) {
        return nearest_provider(inputs.files,
                                providers.modules.find(module_of(name))->getValue(),
                                inputs.file);
    };
    auto distance = [&](llvm::StringRef name) {
        return name.starts_with(":") ? std::uint32_t(0)
                                     : module_distance(inputs.files, nearest(name), inputs.file);
    };
    std::vector<protocol::CompletionItem> items;
    items.reserve(candidates.size());
    for(auto& [name, score]: candidates) {
        protocol::CompletionItem item;
        item.label = name;
        item.kind = protocol::CompletionItemKind::Module;
        // The score is in [0, 2]: a better match sorts first.
        auto mismatch = static_cast<std::uint32_t>((2.0F - score) * 1000.0F);
        item.sort_text = std::format("{:04}{:05}", mismatch, distance(name));
        item.text_edit = protocol::TextEdit{
            .range = *inputs.positions.range(completing.replace, inputs.encoding),
            .new_text = completing.closed ? name : name + ";",
        };
        items.push_back(std::move(item));
    }
    auto list = completion_list(std::move(items), limit);
    for(auto& item: list.items) {
        auto provider = nearest(item.label);
        auto declarations = inputs.query.sites(EntityTable::module_entity(module_of(item.label)),
                                               provider,
                                               RelationKind::Definition);
        auto declaration = ranges::find(declarations, provider, &index::Site::file);
        if(declaration == declarations.end()) {
            continue;
        }
        auto doc = module_documentation(inputs.query, *declaration, inputs.documentation);
        if(doc.empty()) {
            continue;
        }
        auto shown = present(doc);
        item.documentation = protocol::MarkupContent{
            .kind = inputs.client.markdown ? protocol::MarkupKind::Markdown
                                           : protocol::MarkupKind::PlainText,
            .value = inputs.client.markdown ? shown.as_markdown() : shown.as_plain_text(),
        };
    }
    return list;
}

}  // namespace clice::feature
