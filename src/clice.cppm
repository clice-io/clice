/// Primary interface of module clice. Re-exports the third-party wrapper
/// modules (src/deps/) and every partition; implementation units see all of
/// this through their implicit import. Waves append partitions here, sorted
/// by path.
export module clice;

export import stdlib;
export import llvm;
export import clang;

export import :command.argument_parser;
export import :command.command;
export import :command.search_config;
export import :command.toolchain;
export import :support.bitmap;
export import :support.cache_store;
export import :support.doxygen;
export import :support.filesystem;
export import :support.fuzzy_matcher;
export import :support.glob_pattern;
export import :support.markup;
export import :support.object_pool;
export import :support.path_pool;
export import :support.signal;
export import :support.timer;
export import :syntax.completion;
export import :syntax.dependency_graph;
export import :syntax.include_resolver;
export import :syntax.lexer;
export import :syntax.preamble_synthesis;
export import :syntax.scan;
export import :syntax.token;
