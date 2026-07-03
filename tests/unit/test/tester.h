#pragma once

#include <format>
#include <optional>
#include <string>
#include <vector>

#include "test/annotation.h"
#include "test/test.h"
#include "command/command.h"
#include "command/toolchain.h"
#include "compile/compilation.h"
#include "feature/feature.h"
#include "support/logging.h"

namespace clice::testing {

struct Tester {
    CompilationParams params;
    CompilationDatabase database;
    Toolchain toolchain;
    std::optional<CompilationUnit> unit;
    std::string src_path;

    /// Target triple for the VFS-only compile path. Empty means the host
    /// triple. Pin it when the test output must be identical across
    /// platforms, e.g. for snapshot tests.
    std::string triple;

    AnnotatedSources sources;

    /// Owns argument strings so that params.arguments (const char*) remains valid.
    std::vector<std::string> owned_args;

    /// The VFS used for compilation.
    llvm::IntrusiveRefCntPtr<TestVFS> vfs;

    struct ModuleFile {
        std::string filename;
        std::string content;
    };

    std::vector<ModuleFile> module_files;
    std::vector<std::string> pcm_paths;

    ~Tester();

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

    void add_module(llvm::StringRef filename, llvm::StringRef content) {
        module_files.push_back({filename.str(), content.str()});
    }

    /// Fast VFS-only path: uses -cc1 directly, no system headers.
    void prepare(llvm::StringRef standard = "-std=c++20");

    bool compile(llvm::StringRef standard = "-std=c++20");

    bool compile_with_pch(llvm::StringRef standard = "-std=c++20");

    bool compile_with_modules(llvm::StringRef standard = "-std=c++20");

    /// Read a file from disk and compile it directly (no VFS content needed).
    bool compile_file(llvm::StringRef path, llvm::StringRef standard = "-std=c++20");

    /// Driver path: uses CompilationDatabase + toolchain cache, has system headers.
    void prepare_driver(llvm::StringRef standard = "-std=c++20");

    bool compile_driver(llvm::StringRef standard = "-std=c++20");

    bool compile_driver_with_pch(llvm::StringRef standard = "-std=c++20");

    bool try_compile();

    std::uint32_t operator[](llvm::StringRef file, llvm::StringRef pos) {
        return sources.all_files.lookup(file).offsets.lookup(pos);
    }

    std::uint32_t point(llvm::StringRef name = "", llvm::StringRef file = "");

    llvm::ArrayRef<std::uint32_t> nameless_points(llvm::StringRef file = "");

    LocalSourceRange range(llvm::StringRef name = "", llvm::StringRef file = "");

    /// Convert a protocol range back to byte offsets in the compiled unit's
    /// interested file. Shared by feature tests that compare protocol results
    /// against annotation ranges. An unmappable range is a test bug — fail
    /// loudly instead of dereferencing an empty optional.
    LocalSourceRange to_local_range(const kota::ipc::protocol::Range& range) {
        feature::LineMap map(unit->interested_content(),
                             unit->line_starts(),
                             feature::PositionEncoding::UTF8);
        auto begin = map.to_offset(range.start);
        auto end = map.to_offset(range.end);
        if(!begin || !end) {
            LOG_FATAL("to_local_range: unmappable range {}:{}-{}:{}",
                      range.start.line,
                      range.start.character,
                      range.end.line,
                      range.end.character);
        }
        return LocalSourceRange(*begin, *end);
    }

    void clear();
};

inline std::string yaml_str(llvm::StringRef s) {
    std::string result;
    result.reserve(s.size() + 2);
    result += '"';
    for(char c: s) {
        switch(c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if(static_cast<unsigned char>(c) < 0x20) {
                    result += std::format("\\x{:02x}", static_cast<unsigned char>(c));
                } else {
                    result += c;
                }
                break;
        }
    }
    result += '"';
    return result;
}

}  // namespace clice::testing
