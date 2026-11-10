/// Completing a directive's name after its `#`.

module clice:feature.code_completion.directives;

import :feature.code_completion.cursor;
import :feature.code_completion;
import :support.prelude;
import :syntax.position;

namespace clice::feature {

/// The directives a directive name being typed (`completing`) may name,
/// as a compile's completion offers them: `#elif`, `#else` and `#endif`
/// inside a conditional block (CompletionName::conditional). A directive's
/// arguments come as placeholders with statement snippets.
auto directive_completion(const CompletionName& completing,
                          const PositionMap& map,
                          PositionEncoding encoding,
                          const clang::LangOptions& dialect,
                          const CodeCompletionOptions& options,
                          const CompletionClient& client) -> protocol::CompletionList;

}  // namespace clice::feature
