module clice;

import :feature.code_completion.cursor;
import :feature.code_completion.directives;
import :feature.code_completion.include_paths;
import :feature.code_completion.module_names;
import :feature.code_completion;

namespace clice::feature {

auto code_complete(const CompletionInputs& inputs)
    -> std::variant<NoCompletion, protocol::CompletionList, CompileCompletion> {
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
                return module_completion(completing, inputs);
            default: break;
        }
    }

    CompileCompletion compile;
    // A number names nothing.
    if(auto typed = offset ? Prefix::from(text, *offset).spelling : llvm::StringRef();
       completing.directive.empty() && !typed.empty() && !clang::isDigit(typed.front())) {
        compile.prefix = typed.str();
    }
    return compile;
}

}  // namespace clice::feature
