/// Primary interface of module clice. Re-exports the third-party wrapper
/// modules (src/deps/) and every partition; implementation units see all of
/// this through their implicit import. Waves append partitions here, sorted
/// by path.
export module clice;

export import stdlib;
export import llvm;
export import clang;
export import kota;

export import :command.argument_parser;
export import :command.command;
export import :command.search_config;
export import :command.toolchain;
export import :compile.compilation;
export import :compile.compilation_unit;
export import :compile.dep_file;
export import :compile.diagnostic;
export import :compile.directive;
export import :compile.implement;
export import :compile.tidy_checker;
export import :semantic.ast_utility;
export import :semantic.filtered_ast_visitor;
export import :semantic.find_target;
export import :semantic.relation_kind;
export import :semantic.resolver;
export import :semantic.selection;
export import :semantic.semantic_visitor;
export import :semantic.symbol_id;
export import :semantic.symbol_kind;
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
