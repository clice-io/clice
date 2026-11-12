module clice;

import :command.search_config;
import :command.table;
import :feature.code_completion.cursor;
import :feature.code_completion;
import :syntax.lexer;
import :syntax.position;
import :unittests.utils;
import :vfs.dir_cache;
import :vfs.path;

namespace clice::testing {
namespace {

ZEST_SUITE(IncludeCompletion) {

/// Builtin headers not to include directly list last: no case's command searches clice's own.
ZEST_CASE(InternalBuiltinsLast) {
    llvm::StringRef text = "#include <amd";
    std::vector<std::uint32_t> lines = {0};
    PositionMap map(text, lines);
    auto completing = feature::completion_name(text, text.size(), dialect_of());
    SearchConfig config;
    config.dirs.push_back(
        {.path = Spelling::absolute(path::join(resource_dir(), "include")).str(), .driver = true});
    vfs::DirCache cache;
    vfs::Scope scope(cache);
    auto list = feature::include_completion(completing,
                                            text,
                                            map,
                                            PositionEncoding::UTF16,
                                            "",
                                            config,
                                            scope,
                                            0);
    ranges::sort(list.items, {}, [](const protocol::CompletionItem& item) {
        return std::tie(static_cast<const std::optional<std::string>&>(item.sort_text), item.label);
    });
    auto labels =
        ranges::to<std::vector>(list.items | views::transform(&protocol::CompletionItem::label));
    ZEXPECT(labels == std::vector<std::string>{"amdhsa_abi.h", "amdgpuintrin.h"});
}

};  // ZEST_SUITE(IncludeCompletion)

}  // namespace
}  // namespace clice::testing
