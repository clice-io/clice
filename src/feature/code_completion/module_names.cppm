/// Completing the module name of an `import` or of an implementation
/// unit's module declaration.

module clice:feature.code_completion.module_names;

import :feature.code_completion.cursor;
import :feature.code_completion;
import :support.prelude;

namespace clice::feature {

/// The modules an `import` or a module declaration being typed
/// (`completing`) may name, each with its documentation: the better the
/// name typed matches, the earlier; then the unit's own partitions first,
/// then the modules whose interface lies nearest the file — the fewest
/// directories up from it to one holding the interface.
/// The other open units provide what their buffers declare, the file what
/// its text does.
auto module_completion(const CompletionName& completing, const CompletionInputs& inputs)
    -> protocol::CompletionList;

}  // namespace clice::feature
