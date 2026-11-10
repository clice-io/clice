/// What a completion item adds for the name it inserts to be reachable:
/// the `#include` of its header, which the server adds to a compile's
/// reply (finish_completion), or the `import` of its module, which the
/// compile adds itself.

module clice:feature.code_completion.insertion;

import :support.prelude;

namespace clice::feature {

/// The `import` declaration a unit of the module `unit` (`name` or
/// `name:partition`, empty outside a named module) needs to reach what the
/// module `module` exports; none when the unit is that module.
auto import_for(llvm::StringRef module, llvm::StringRef unit) -> std::optional<std::string>;

}  // namespace clice::feature
