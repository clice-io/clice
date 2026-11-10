module clice;

import :feature.code_completion;
import :index.query;
import :index.search;
import :index.symbol;
import :semantic.symbol;
import :semantic.token_class;
import :syntax.dependency_graph;

namespace clice::feature {

void offer_names(const index::IndexQuery& query,
                 const DependencyGraph& graph,
                 CompileCompletion& compile) {
    if(!compile.prefix) {
        return;
    }
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
    namespace stdlib = clang::tooling::stdlib;
    bool builds_std = !graph.lookup_module("std").empty();
    auto found = index::completables(query, *compile.prefix, scope_candidate_limit);
    for(auto& [symbol, chain, declared]: found.names) {
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
        if(builds_std && candidate.module.empty() && !candidate.scope.empty() &&
           candidate.scope.front() == "std") {
            std::string qualifier;
            for(const auto& segment: candidate.scope) {
                qualifier += segment + "::";
            }
            candidate.std_module =
                stdlib::Symbol::named(qualifier, candidate.name, stdlib::Lang::CXX).has_value();
        }
        compile.candidates.push_back(std::move(candidate));
    }
    compile.more_candidates = !found.exhausted;
}

}  // namespace clice::feature
