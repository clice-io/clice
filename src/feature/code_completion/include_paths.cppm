/// Completing the header name of an `#include` or a `__has_include`.

module clice:feature.code_completion.include_paths;

import :command.search_config;
import :feature.code_completion.cursor;
import :support.prelude;
import :syntax.position;
import :vfs.dir_cache;

namespace clice::feature {

/// The paths an `#include` being typed (`completing`) may go on with in
/// `text`, the file at `path`, as `search` finds them: a directory leaves
/// the path open for the `/` typed next, a header closes the directive.
auto include_completion(const CompletionName& completing,
                        llvm::StringRef text,
                        const PositionMap& map,
                        PositionEncoding encoding,
                        llvm::StringRef path,
                        const SearchConfig& search,
                        vfs::Scope& scope,
                        std::uint32_t limit) -> protocol::CompletionList;

}  // namespace clice::feature
