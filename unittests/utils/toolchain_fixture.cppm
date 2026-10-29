module clice:unittests.utils.toolchain_fixture;

import :command.database;
import :command.edit;
import :command.table;
import :command.toolchain;
import :support.prelude;
import :unittests.utils.temp_dir;
import :vfs.file_system;
import :vfs.file_table;

namespace clice::testing {

/// A toolchain over a database of test entries — the unit tests' handle on
/// the two toolchain layers.
struct ToolchainEntries {
    FileTable file_table;

    CommandTable commands;

    CompilationDatabase db{file_table, commands};

    Toolchain toolchain{commands, file_table};

    CommandRef add(llvm::StringRef directory,
                   llvm::StringRef file,
                   llvm::ArrayRef<const char*> arguments) {
        db.add_command(directory, file, arguments);
        auto& entry = db.candidate_entries(file).front();
        return {entry.file,
                entry.command,
                commands.input_kind(entry.command, file),
                CommandSource::CDBExact};
    }

    std::string key(const CommandRef& ref) {
        return toolchain.query_key_for(ref.command, ref.input);
    }

    /// The command clice compiles `ref` with, resolved through the query.
    std::vector<const char*> resolve(const CommandRef& ref) {
        ZASSERT(toolchain.resolve(ref.command, ref.input));
        return toolchain.render(ref, db.spelling(ref.file)).argv;
    }

    /// Why the compiler of `ref` could not be queried, which leaves clang's
    /// own driver reading its command.
    std::string failure(const CommandRef& ref) {
        auto resolution = toolchain.resolve(ref.command, ref.input);
        ZASSERT(resolution);
        ZASSERT(!resolution->query_failure.empty());
        return resolution->query_failure;
    }
};

/// The toolchain tests' fixture: entries in a temporary directory, where
/// shell scripts stand in for compiler drivers.
struct ToolchainFixture {
    TempDir tmp;

    ToolchainEntries f;

    /// `arguments`, then a source file named `file` in the temp directory, as
    /// that file's command.
    CommandRef add(llvm::StringRef file, std::vector<const char*> arguments) {
        arguments.push_back(tmp.c_path(file));
        return f.add(tmp.root, tmp.path(file), arguments);
    }

    /// An executable shell script `name` in the temp directory, standing in
    /// for a compiler driver.
    std::string fake_driver(llvm::StringRef name, llvm::StringRef script) {
        auto driver = tmp.path(name);
        ZASSERT(!fs::write_file(driver, "#!/bin/sh\n" + script.str()));
        ZASSERT(!fs::set_permissions(driver, 0555));
        return driver;
    }

    /// A clang driver that prints `job` as the job it runs, and C++14's macro.
    std::string fake_clang(llvm::StringRef job, llvm::StringRef name = "clang") {
        return fake_driver(name,
                           std::format(R"(echo '{}' >&2
echo '#define __cplusplus 201402L'
)",
                                       job));
    }

    /// A GCC driver that prints `output` on stderr, and C++14's macro.
    std::string fake_gcc(llvm::StringRef name, llvm::StringRef output) {
        return fake_driver(name,
                           std::format(R"(cat >&2 <<'OUTPUT'
{}
OUTPUT
echo '#define __cplusplus 201402L'
)",
                                       output));
    }

    /// An nvcc whose dryrun names `host` as its host compiler.
    std::string fake_nvcc(llvm::StringRef host) {
        return fake_driver("nvcc",
                           std::format(R"(echo '#$ {} -c' >&2
)",
                                       host));
    }

    /// `ref` under a rule turning the toolchain query off.
    CommandRef unqueried(const CommandRef& ref) {
        return {ref.file,
                apply_edits(f.commands, f.file_table, ref.command, {.query_toolchain = false}),
                ref.input,
                ref.source};
    }
};

/// A clang job as `-v` prints it.
constexpr inline llvm::StringRef clang_job =
    R"( "/usr/bin/clang-22" "-cc1" "-triple" "x86_64-unknown-linux-gnu" "-std=c++23")";

/// Whether `argv` holds `first` directly followed by `second`.
inline bool adjacent(llvm::ArrayRef<const char*> argv,
                     llvm::StringRef first,
                     llvm::StringRef second) {
    for(std::size_t i = 0; i + 1 < argv.size(); i += 1) {
        if(argv[i] == first && argv[i + 1] == second) {
            return true;
        }
    }
    return false;
}

/// Whether `argv`, of `const char*` or of strings, holds `arg`.
inline bool contains(const auto& argv, llvm::StringRef arg) {
    return ranges::contains(argv, arg);
}

}  // namespace clice::testing
