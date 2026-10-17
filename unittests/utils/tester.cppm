module;

#include "modules/prelude.h"

module clice:unittests.utils.tester;

import :command.command;
import :compile.compilation;
import :unittests.utils.annotation;
import :unittests.utils.temp_dir;
import :unittests.utils;

namespace clice::testing {

struct Tester {
    /// Where the PCHs and PCMs of the test are written; removed with the
    /// tester, after the units that write them when destroyed.
    TempDir artifacts{"clice-artifacts"};

    CompilationParams params;
    FileTable file_table;
    CompilationDatabase database{file_table};
    std::unique_ptr<CompilationUnit> unit;
    std::string src_path;

    /// Target triple for the VFS-only compile path. Empty means the host
    /// triple. Pin it when the test output must be identical across
    /// platforms, e.g. for snapshot tests.
    std::string triple;

    /// What compile() builds the focus file as: an open file's compile, or
    /// the background index's, whose index covers every file.
    CompilationKind kind = CompilationKind::Content;

    AnnotatedSources sources;

    /// The VFS used for compilation.
    llvm::IntrusiveRefCntPtr<TestVFS> vfs;

    struct ModuleFile {
        std::string filename;
        std::string content;
    };

    std::vector<ModuleFile> module_files;

    /// How many paths artifact() handed out.
    std::uint32_t artifact_count = 0;

    /// A path in `artifacts` for a file with this extension, one no other
    /// call returns.
    std::string artifact(llvm::StringRef extension) {
        artifact_count += 1;
        return artifacts.path(std::format("{}.{}", artifact_count, extension));
    }

    void add_focus(llvm::StringRef file, llvm::StringRef content) {
        src_path = file.str();
        sources.add_source(file, content);
    }

    void add_file(llvm::StringRef name, llvm::StringRef content) {
        sources.add_source(name, content);
    }

    void add_files(llvm::StringRef focus, llvm::StringRef content) {
        src_path = focus.str();
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

    /// Driver path: uses CompilationDatabase + toolchain cache, has system headers.
    void prepare_driver(llvm::StringRef standard = "-std=c++20");

    bool compile_driver(llvm::StringRef standard = "-std=c++20");

    bool try_compile();

    std::uint32_t operator[](llvm::StringRef file, llvm::StringRef pos) {
        return sources.all_files.lookup(file).offsets.lookup(pos);
    }

    std::uint32_t point(llvm::StringRef name = "", llvm::StringRef file = "");

    llvm::ArrayRef<std::uint32_t> nameless_points(llvm::StringRef file = "");

    LocalSourceRange range(llvm::StringRef name = "", llvm::StringRef file = "");

    /// Reset per-compile state so one case can compile several sources.
    /// The database persists — entries are append-only and harmless.
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
