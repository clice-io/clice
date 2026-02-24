#pragma once

#include <optional>
#include <string>

#include "test/annotation.h"
#include "test/test.h"
#include "compile/command.h"
#include "compile/compilation.h"
#include "eventide/language/protocol.h"
#include "support/logging.h"

namespace clice::testing {

struct Tester {
    CompilationParams params;
    CompilationDatabase database;
    std::optional<CompilationUnit> unit;
    std::string src_path;

    AnnotatedSources sources;

    void add_main(llvm::StringRef file, llvm::StringRef content) {
        src_path = file.str();
        sources.add_source(file, content);
    }

    void add_file(llvm::StringRef name, llvm::StringRef content) {
        sources.add_source(name, content);
    }

    void add_files(llvm::StringRef main_file, llvm::StringRef content) {
        src_path = main_file.str();
        sources.add_sources(content);
    }

    void prepare(llvm::StringRef standard = "-std=c++20");

    bool compile(llvm::StringRef standard = "-std=c++20");

    bool compile_with_pch(llvm::StringRef standard = "-std=c++20");

    std::uint32_t operator[](llvm::StringRef file, llvm::StringRef pos) {
        return sources.all_files.lookup(file).offsets.lookup(pos);
    }

    std::uint32_t point(llvm::StringRef name = "", llvm::StringRef file = "");

    llvm::ArrayRef<std::uint32_t> nameless_points(llvm::StringRef file = "");

    LocalSourceRange range(llvm::StringRef name = "", llvm::StringRef file = "");

    void clear();
};

}  // namespace clice::testing
