module clice;

import :command.search_config;
import :command.table;
import :feature.code_completion.cursor;
import :feature.code_completion;
import :support.json;
import :syntax.include_resolver;
import :syntax.lexer;
import :syntax.position;
import :unittests.utils.temp_dir;
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
                                            at_most(NoLimit::Unlimited));
    ranges::sort(list.items, {}, [](const protocol::CompletionItem& item) {
        return std::tie(static_cast<const std::optional<std::string>&>(item.sort_text), item.label);
    });
    auto labels =
        ranges::to<std::vector>(list.items | views::transform(&protocol::CompletionItem::label));
    ZEXPECT(labels == std::vector<std::string>{"amdhsa_abi.h", "amdgpuintrin.h"});
}

};  // ZEST_SUITE(IncludeCompletion)

ZEST_SUITE(IncludeArgument) {

/// A pattern asking for quotes changes a header -isystem finds, never one
/// of the toolchain's own.
ZEST_CASE(ToolchainHeadersAngled) {
    TempDir tmp;
    tmp.touch("vendor/acme.h");
    tmp.touch("toolchain/limits.h");
    tmp.touch("src/main.cpp");

    SearchConfig config;
    config.dirs.push_back({tmp.path("vendor")});
    config.dirs.push_back({.path = tmp.path("toolchain"), .driver = true});

    feature::IncludeDelimiters delimiters;
    auto everything = kotatsu::GlobPattern::create("**");
    ZASSERT(everything);
    delimiters.quoted.push_back(std::move(*everything));

    vfs::DirCache cache;
    vfs::Scope scope(cache);
    IncludeNames names(config, Spelling::absolute(tmp.path("src/main.cpp")), scope);
    auto argument = [&](llvm::StringRef path) {
        return feature::include_argument(names,
                                         delimiters,
                                         CanonicalPath(Spelling::absolute(tmp.path(path))));
    };
    ZEXPECT(argument("vendor/acme.h") == std::optional<std::string>(R"("acme.h")"));
    ZEXPECT(argument("toolchain/limits.h") == std::optional<std::string>("<limits.h>"));
}

};  // ZEST_SUITE(IncludeArgument)

}  // namespace
}  // namespace clice::testing
