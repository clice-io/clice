/// Primary interface of module clice. Re-exports the third-party wrapper
/// modules (src/deps/) and every partition; implementation units see all of
/// this through their implicit import. Waves append partitions here, sorted
/// by path.
export module clice;

export import stdlib;
export import llvm;
export import clang;

export import :support.fuzzy_matcher;
