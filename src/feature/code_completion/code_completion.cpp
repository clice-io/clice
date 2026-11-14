module clice;

import :feature.code_completion.cursor;
import :feature.code_completion;
import :index.query;
import :index.query.locate;
import :index.symbol;
import :semantic.symbol;
import :semantic.token_class;
import :syntax.dependency_graph;
import :syntax.source_text;
import :vfs.dir_cache;
import :vfs.file_table;

namespace clice::feature {

/// The project's names the index offers for the name `typed`
/// (ScopeCandidate), of the scope a qualifier names when one is written,
/// with whether it holds more.
static auto offer_names(const index::IndexQuery& query,
                        const DependencyGraph& graph,
                        llvm::StringRef typed,
                        const std::optional<WrittenScope>& written) -> CompileCompletion {
    CompileCompletion compile;
    auto kind_of = [](SymbolKind kind) {
        using Kind = protocol::CompletionItemKind;
        switch(kind) {
            case SymbolKind::Namespace: return Kind::Module;
            case SymbolKind::Class:
            case SymbolKind::Struct:
            case SymbolKind::Union: return Kind::Class;
            case SymbolKind::Enum: return Kind::Enum;
            case SymbolKind::Protocol: return Kind::Interface;
            case SymbolKind::Property: return Kind::Property;
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
    auto found = written ? index::completables(query,
                                               typed,
                                               scope_candidate_limit,
                                               written->segments,
                                               written->global)
                         : index::completables(query, typed, scope_candidate_limit);
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
    return compile;
}

auto code_complete(const CompletionInputs& inputs)
    -> std::variant<NoCompletion, protocol::CompletionList, CompileCompletion> {
    auto text = inputs.source.text();
    auto& dialect = inputs.source.dialect();
    auto positions = inputs.source.positions();
    auto offset = positions.offset(inputs.params.position, inputs.encoding);
    CompletionName completing;
    if(offset) {
        completing = completion_name(text, *offset, dialect);
    }
    llvm::StringRef trigger;
    if(auto& context = inputs.params.context; context && context->trigger_character) {
        trigger = *context->trigger_character;
    }

    // Anything else a trigger character is typed in — `template<`, a
    // closing `>`, the dots of a pack ellipsis — is answered before any
    // include scanning or completion build.
    if(!trigger.empty()) {
        bool served = trigger == " "
                          ? completing.kind == CompletionName::Kind::Import ||
                                completing.kind == CompletionName::Kind::ModuleDeclaration
                          : completing.kind != CompletionName::Kind::None || !offset ||
                                follows_access_operator(text, *offset, dialect);
        if(!served) {
            return NoCompletion{};
        }
    }

    switch(completing.kind) {
        case CompletionName::Kind::Directive:
            return directive_completion(completing,
                                        positions,
                                        inputs.encoding,
                                        dialect,
                                        inputs.options,
                                        inputs.client);
        case CompletionName::Kind::IncludeQuoted:
        case CompletionName::Kind::IncludeAngled: {
            auto& files = inputs.query.file_table();
            vfs::Scope scope(files.dirs);
            return include_completion(completing,
                                      text,
                                      positions,
                                      inputs.encoding,
                                      std::string(files.resolve(inputs.file)),
                                      inputs.search(),
                                      scope,
                                      inputs.options.limit);
        }
        case CompletionName::Kind::Import:
        case CompletionName::Kind::ModuleDeclaration: return module_completion(completing, inputs);
        default: break;
    }

    // A number names nothing.
    // LIMITATION: the index is asked for the scope as written, before the
    // compile resolves it: after a namespace alias (`fs::pa`), a name of the
    // aliased namespace that a header the file does not include declares is
    // not offered.
    if(auto prefix = offset ? std::optional(Prefix::from(text, *offset)) : std::nullopt;
       prefix && inputs.options.all_scopes && completing.directive.empty() &&
       !prefix->spelling.empty() && !clang::isDigit(prefix->spelling.front())) {
        return offer_names(inputs.query,
                           inputs.graph,
                           prefix->spelling,
                           written_scope(text, prefix->typed.begin, dialect));
    }
    return CompileCompletion{};
}

}  // namespace clice::feature
