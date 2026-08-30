#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "command/argument_parser.h"
#include "command/command.h"
#include "support/filesystem.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::testing {

namespace {

using namespace std::literals;

#define EXPECT_CONTAINS(haystack, needle) EXPECT_TRUE(llvm::StringRef(haystack).contains(needle))
#define EXPECT_NOT_CONTAINS(haystack, needle)                                                      \
    EXPECT_FALSE(llvm::StringRef(haystack).contains(needle))

TEST_SUITE(Command) {

/// The synthesized fallback render for a file without an entry, resource
/// dir stripped like render_entry.
std::vector<const char*> render_fallback(CompilationDatabase& db,
                                         llvm::StringRef file,
                                         const CommandOptions& options = {}) {
    auto applied = db.apply_rules(db.fallback_config(file), options);
    CommandRef ref{db.paths().intern(file),
                   applied,
                   db.input_kind(applied, file),
                   CommandSource::Fallback};
    auto argv = db.render_driver(ref);
    for(std::size_t i = 0; i + 1 < argv.size(); i += 1) {
        if(llvm::StringRef(argv[i]) == "-resource-dir") {
            argv.erase(argv.begin() + i, argv.begin() + i + 2);
            break;
        }
    }
    return argv;
}

void EXPECT_STRIP(llvm::StringRef argv, llvm::StringRef result) {
    CompilationDatabase database;
    llvm::StringRef file = "main.cpp";
    database.add_command("fake/", file, argv);
    ASSERT_EQ(result, print_argv(render_entry(database, file)));
};

TEST_CASE(DefaultFilters) {
    /// Filter -c, -o and keep the input in place.
    EXPECT_STRIP("g++ main.cpp", "g++ main.cpp");
    EXPECT_STRIP("clang++ -c main.cpp", "clang++ main.cpp");
    EXPECT_STRIP("clang++ -o main.o main.cpp", "clang++ main.cpp");
    EXPECT_STRIP("clang++ -c -o main.o main.cpp", "clang++ main.cpp");
    EXPECT_STRIP("cl.exe /c /Fomain.cpp.o main.cpp", "cl.exe main.cpp");
    /// CL options stay visible under Windows's free-form driver casing.
    EXPECT_STRIP("CL.exe /FIfoo.h /c main.cpp", "CL.exe -include foo.h main.cpp");

    /// Filter PCH related.

    /// CMake
    EXPECT_STRIP("g++ -std=gnu++20 -Winvalid-pch -include cmake_pch.hxx -o main.cpp.o -c main.cpp",
                 "g++ -std=gnu++20 -Winvalid-pch -include cmake_pch.hxx main.cpp");
    EXPECT_STRIP(
        "clang++ -Winvalid-pch -Xclang -include-pch -Xclang cmake_pch.hxx.pch -Xclang -include -Xclang cmake_pch.hxx -o main.cpp.o -c main.cpp",
        "clang++ -Winvalid-pch -Xclang -include -Xclang cmake_pch.hxx main.cpp");
    EXPECT_STRIP("cl.exe /Yufoo.h /FIfoo.h /Fpfoo.h_v143.pch /c /Fomain.cpp.o main.cpp",
                 "cl.exe -include foo.h main.cpp");
};

TEST_CASE(ConfigDedup) {
    CompilationDatabase database;
    database.add_command("fake", "test.cpp", "clang++ -std=c++23 test.cpp"sv);
    database.add_command("fake", "test2.cpp", "clang++ -std=c++23 test2.cpp"sv);
    database.add_command("fake", "test3.cpp", "clang++ -std=c++23 -DA test3.cpp"sv);

    /// Same flags dedupe to one config; a user-content difference splits.
    auto config1 = database.candidate_entries("test.cpp").front().config;
    auto config2 = database.candidate_entries("test2.cpp").front().config;
    auto config3 = database.candidate_entries("test3.cpp").front().config;
    EXPECT_EQ(config1, config2);
    EXPECT_NE(config1, config3);

    auto argv1 = render_entry(database, "test.cpp");
    ASSERT_EQ(argv1.size(), 3U);
    EXPECT_EQ(argv1[0], "clang++"sv);
    EXPECT_EQ(argv1[1], "-std=c++23"sv);
    EXPECT_EQ(argv1[2], "test.cpp"sv);
    EXPECT_EQ(render_entry(database, "test2.cpp").back(), "test2.cpp"sv);
};

TEST_CASE(RemoveAppend) {
    llvm::SmallVector args = {
        "clang++",
        "--output=main.o",
        "-D",
        "A",
        "-D",
        "B=0",
        "main.cpp",
    };

    CompilationDatabase database;
    database.add_command("/fake", "main.cpp", args);

    CommandOptions options;

    llvm::SmallVector<std::string> remove;
    llvm::SmallVector<std::string> append;

    remove = {"-DA"};
    options.remove = remove;
    EXPECT_EQ(print_argv(render_entry(database, "main.cpp", options)), "clang++ -D B=0 main.cpp");

    remove = {"-D", "A"};
    options.remove = remove;
    EXPECT_EQ(print_argv(render_entry(database, "main.cpp", options)), "clang++ -D B=0 main.cpp");

    remove = {"-DA", "-D", "B=0"};
    options.remove = remove;
    EXPECT_EQ(print_argv(render_entry(database, "main.cpp", options)), "clang++ main.cpp");

    remove = {"-D*"};
    options.remove = remove;
    EXPECT_EQ(print_argv(render_entry(database, "main.cpp", options)), "clang++ main.cpp");

    remove = {"-D", "*"};
    options.remove = remove;
    EXPECT_EQ(print_argv(render_entry(database, "main.cpp", options)), "clang++ main.cpp");

    options.remove = {};
    append = {"-D", "C"};
    options.append = append;
    EXPECT_EQ(print_argv(render_entry(database, "main.cpp", options)),
              "clang++ -D A -D B=0 -D C main.cpp");
};

TEST_CASE(AppendUnknownValue) {
    /// An appended option the table does not know keeps its separate value:
    /// an edit cannot name the entry input, so an input-classified token is
    /// really the option's value.
    CompilationDatabase database;
    database.add_command("/fake", "main.cpp", "clang++ main.cpp"sv);

    CommandOptions options;
    llvm::SmallVector<std::string> append = {"-fnot-a-real-flag", "value"};
    options.append = append;
    EXPECT_EQ(print_argv(render_entry(database, "main.cpp", options)),
              "clang++ -fnot-a-real-flag value main.cpp");
};

TEST_CASE(AppendBeforeSlot) {
    /// Appends insert before the input slot, so they always govern the
    /// compile — even when the CDB command carries flags after the input.
    CompilationDatabase database;
    database.add_command("/fake", "a.c", "clang -x c a.c -x none"sv);

    CommandOptions options;
    llvm::SmallVector<std::string> append = {"-x", "c++"};
    options.append = append;

    EXPECT_EQ(print_argv(render_entry(database, "a.c", options)), "clang -x c -x c++ a.c -x none");

    auto applied = database.apply_rules(database.candidate_entries("a.c").front().config, options);
    EXPECT_EQ(llvm::StringRef(database.input_kind(applied, "a.c").value), "c++");
};

TEST_CASE(SelectorHistoryRestored) {
    /// Removing the later selector re-exposes the earlier one.
    CompilationDatabase database;
    database.add_command("/fake", "a.c", "clang -x cuda -x c++ a.c"sv);

    auto base = database.candidate_entries("a.c").front().config;
    EXPECT_EQ(llvm::StringRef(database.input_kind(base, "a.c").value), "c++");

    CommandOptions options;
    llvm::SmallVector<std::string> remove = {"-x", "c++"};
    options.remove = remove;
    auto applied = database.apply_rules(base, options);
    EXPECT_EQ(llvm::StringRef(database.input_kind(applied, "a.c").value), "cuda");
};

TEST_CASE(SelectorPositional) {
    /// -x only governs inputs after it: a trailing selector leaves the
    /// input to its extension, and removing the leading one restores it.
    CompilationDatabase database;
    database.add_command("/fake", "a.cu", "clang -x c++ a.cu -x c"sv);

    auto base = database.candidate_entries("a.cu").front().config;
    EXPECT_EQ(llvm::StringRef(database.input_kind(base, "a.cu").value), "c++");

    CommandOptions options;
    llvm::SmallVector<std::string> remove = {"-x", "c++"};
    options.remove = remove;
    auto applied = database.apply_rules(base, options);
    EXPECT_EQ(llvm::StringRef(database.input_kind(applied, "a.cu").value), "cuda");

    /// -x none resets the state; the extension decides again.
    database.add_command("/fake", "b.c", "clang -x c++ -x none b.c"sv);
    auto reset = database.candidate_entries("b.c").front().config;
    EXPECT_EQ(llvm::StringRef(database.input_kind(reset, "b.c").value), "c");
};

TEST_CASE(PerFileClSelectors) {
    /// /Tc<file> and /Tp<file> pair a selector with one input: the entry's
    /// own selector rewrites to the equivalent global form, the other
    /// input vanishes with its selector.
    CompilationDatabase database;
    database.add_command("/fake", "/fake/alpha.c", "cl /Tc/fake/alpha.c /Tp/fake/beta.c /c"sv);
    database.add_command("/fake", "/fake/beta.c", "cl /Tc/fake/alpha.c /Tp/fake/beta.c /c"sv);

    auto alpha = database.candidate_entries("/fake/alpha.c").front().config;
    auto beta = database.candidate_entries("/fake/beta.c").front().config;
    EXPECT_NE(alpha, beta);

    EXPECT_EQ(llvm::StringRef(database.input_kind(alpha, "/fake/alpha.c").value), "c");
    EXPECT_EQ(llvm::StringRef(database.input_kind(beta, "/fake/beta.c").value), "c++");

    auto beta_argv = print_argv(render_entry(database, "/fake/beta.c"));
    EXPECT_CONTAINS(beta_argv, "/TP");
    EXPECT_NOT_CONTAINS(beta_argv, "alpha.c");

    auto alpha_argv = print_argv(render_entry(database, "/fake/alpha.c"));
    EXPECT_CONTAINS(alpha_argv, "/TC");
    EXPECT_NOT_CONTAINS(alpha_argv, "beta.c");
};

TEST_CASE(IdentityHashes) {
    CompilationDatabase database;
    database.add_command("/fake", "a.cpp", "clang++ -std=c++20 a.cpp"sv);
    database.add_command("/fake", "b.cpp", "clang++ -std=c++20 b.cpp"sv);
    database.add_command("/fake", "c.cpp", "clang++ -std=c++17 c.cpp"sv);
    database.add_command("/fake", "x1.c", "clang -x c++ x1.c"sv);
    database.add_command("/fake", "x2.c", "clang -x c x2.c"sv);
    database.add_command("/fake", "g.cpp", "clang++ -std=c++20 -g a.cpp"sv);

    auto hash_of = [&](llvm::StringRef file) {
        return database.entry_hash(database.candidate_entries(file).front().config);
    };

    /// Same command, different file: one config, one identity.
    EXPECT_EQ(hash_of("a.cpp"), hash_of("b.cpp"));
    /// A semantic flag difference changes the identity.
    EXPECT_NE(hash_of("a.cpp"), hash_of("c.cpp"));
    /// A selector difference changes the identity (selectors live in args).
    EXPECT_NE(hash_of("x1.c"), hash_of("x2.c"));
    /// Codegen-only flags never enter the identity.
    EXPECT_EQ(hash_of("a.cpp"), hash_of("g.cpp"));

    /// The input slot's position is part of the identity.
    database.add_command("/fake", "p1.cpp", "clang++ p1.cpp -Wall"sv);
    database.add_command("/fake", "p2.cpp", "clang++ -Wall p2.cpp"sv);
    EXPECT_NE(hash_of("p1.cpp"), hash_of("p2.cpp"));

    EXPECT_EQ(database.entry_hash_hex(database.candidate_entries("a.cpp").front().config).size(),
              16U);
};

TEST_CASE(WrapperStripped) {
    CompilationDatabase database;
    database.add_command("/fake", "a.cpp", "ccache clang++ -std=c++20 a.cpp"sv);
    database.add_command("/fake", "b.cpp", "clang++ -std=c++20 b.cpp"sv);

    /// The wrapper is entry provenance, not config identity.
    auto& a = database.candidate_entries("a.cpp").front();
    auto& b = database.candidate_entries("b.cpp").front();
    EXPECT_EQ(a.config, b.config);
    ASSERT_EQ(a.wrapper.size(), 1U);
    EXPECT_EQ(llvm::StringRef(a.wrapper[0]), "ccache");
    EXPECT_TRUE(b.wrapper.empty());

    EXPECT_EQ(llvm::StringRef(database.config(a.config).driver), "clang++");
    EXPECT_NOT_CONTAINS(print_argv(render_entry(database, "a.cpp")), "ccache");
};

TEST_CASE(WrapperValueOptions) {
    /// A wrapper option's separate KEY=VAL value must not be mistaken for
    /// the compiler.
    CompilationDatabase database;
    database.add_command("/fake",
                         "a.cpp",
                         "ccache --set-config cache_dir=/tmp/cc clang++ -std=c++20 a.cpp"sv);
    database.add_command("/fake", "b.cpp", "clang++ -std=c++20 b.cpp"sv);

    auto& a = database.candidate_entries("a.cpp").front();
    auto& b = database.candidate_entries("b.cpp").front();
    EXPECT_EQ(a.config, b.config);
    EXPECT_EQ(llvm::StringRef(database.config(a.config).driver), "clang++");
    ASSERT_EQ(a.wrapper.size(), 3U);
};

TEST_CASE(WrapperCaseInsensitive) {
    /// Windows tools emit launcher spellings like CCACHE.EXE.
    CompilationDatabase database;
    database.add_command("/fake", "a.cpp", "CCACHE.EXE clang++ -std=c++20 a.cpp"sv);

    auto& a = database.candidate_entries("a.cpp").front();
    EXPECT_EQ(llvm::StringRef(database.config(a.config).driver), "clang++");
    ASSERT_EQ(a.wrapper.size(), 1U);
    EXPECT_EQ(llvm::StringRef(a.wrapper[0]), "CCACHE.EXE");
};

TEST_CASE(InputKindNoExtension) {
    /// An extensionless file must yield a real (non-null) empty kind, not
    /// a null C string.
    CompilationDatabase database;
    database.add_command("/fake", "noext", "clang++ -std=c++20 noext"sv);

    auto& entry = database.candidate_entries("noext").front();
    auto kind = database.input_kind(entry.config, "noext");
    ASSERT_TRUE(kind.value != nullptr);
    EXPECT_TRUE(llvm::StringRef(kind.value).empty());
};

TEST_CASE(ResponseFileExpansion) {
    TempDir tmp;
    tmp.touch("flags.rsp", "-std=c++23 -DFROM_RSP=1\n");
    CompilationDatabase database;
    database.add_command(tmp.root.str(), "main.cpp", "clang++ @flags.rsp main.cpp"sv);

    auto argv = print_argv(render_entry(database, "main.cpp"));
    EXPECT_CONTAINS(argv, "-std=c++23");
    EXPECT_CONTAINS(argv, "FROM_RSP=1");
    EXPECT_NOT_CONTAINS(argv, "@");
};

TEST_CASE(DriverModeFromRsp) {
    /// --driver-mode=cl inside a response file still switches on CL option
    /// visibility (clang interprets the mode after expansion).
    TempDir tmp;
    tmp.touch("flags.rsp", "--driver-mode=cl /TP\n");
    CompilationDatabase database;
    database.add_command(tmp.root.str(), "main.cpp", "clang++ @flags.rsp main.cpp"sv);

    EXPECT_CONTAINS(print_argv(render_entry(database, "main.cpp")), "/TP");
};

TEST_CASE(PrependAfterBinary) {
    llvm::SmallVector args = {"clang++", "-DA", "main.cpp"};

    CompilationDatabase database;
    database.add_command("/fake", "main.cpp", args);

    CommandOptions options;
    llvm::SmallVector<std::string> prepend = {"-std=c++17", "-DB"};
    options.extra_prepend = prepend;
    // Prepends sit ahead of the command's own flags, so the command wins
    // on collision. (Defines render canonicalized, as two tokens.)
    EXPECT_EQ(print_argv(render_entry(database, "main.cpp", options)),
              "clang++ -std=c++17 -D B -D A main.cpp");
};

TEST_CASE(DefaultFallback) {
    CompilationDatabase database;

    /// C++ files get "clang++ -std=c++20 <file>".
    auto cpp_argv = render_fallback(database, "unknown.cpp");
    ASSERT_EQ(cpp_argv.size(), 3U);
    EXPECT_EQ(cpp_argv[0], "clang++"sv);
    EXPECT_EQ(cpp_argv[1], "-std=c++20"sv);
    EXPECT_EQ(cpp_argv[2], "unknown.cpp"sv);

    /// .hpp and .cc also get the C++ default.
    EXPECT_EQ(render_fallback(database, "header.hpp")[0], "clang++"sv);
    EXPECT_EQ(render_fallback(database, "file.cc")[0], "clang++"sv);

    /// C files get "clang <file>".
    auto c_argv = render_fallback(database, "unknown.c");
    ASSERT_EQ(c_argv.size(), 2U);
    EXPECT_EQ(c_argv[0], "clang"sv);
    EXPECT_EQ(c_argv[1], "unknown.c"sv);

    /// Other extensions also get plain clang.
    EXPECT_EQ(render_fallback(database, "foo.h")[0], "clang"sv);

    /// CUDA files pin cuda mode and the device-side view NVCC-backed
    /// commands default to (the render spells the unaliased form).
    for(llvm::StringRef cuda_file: {"kern.cu", "kernels.cuh"}) {
        auto cu_argv = render_fallback(database, cuda_file);
        ASSERT_EQ(cu_argv.size(), 6U);
        EXPECT_EQ(cu_argv[0], "clang++"sv);
        EXPECT_EQ(cu_argv[2], "-x"sv);
        EXPECT_EQ(cu_argv[3], "cuda"sv);
        EXPECT_EQ(cu_argv[4], "--offload-device-only"sv);
    }
};

TEST_CASE(FallbackAppliesAppend) {
    /// Config rule appends must reach the synthesized fallback command:
    /// users without a CDB rely on them to supply include paths.
    CompilationDatabase database;
    CommandOptions options;
    std::vector<std::string> append = {"-I/opt/include"};
    options.append = append;

    auto argv = print_argv(render_fallback(database, "unknown.cpp", options));
    EXPECT_CONTAINS(argv, "-std=c++20");
    EXPECT_CONTAINS(argv, "/opt/include");

    /// The plain-clang branch applies them too.
    EXPECT_CONTAINS(print_argv(render_fallback(database, "unknown.c", options)), "/opt/include");
};

TEST_CASE(MultiCommand) {
    /// A file can have multiple compilation commands (e.g. different configs).
    CompilationDatabase database;
    database.add_command("fake", "main.cpp", "clang++ -std=c++17 main.cpp"sv);
    database.add_command("fake", "main.cpp", "clang++ -std=c++20 main.cpp"sv);
    database.add_command("fake", "other.cpp", "clang++ -std=c++23 other.cpp"sv);

    auto candidates = database.candidate_entries("main.cpp");
    ASSERT_EQ(candidates.size(), 2U);

    /// Both commands are present, in a stable content-based order.
    bool has_17 = false, has_20 = false;
    for(auto& entry: candidates) {
        auto argv = print_argv(database.render_full(entry.config));
        if(llvm::StringRef(argv).contains("-std=c++17"))
            has_17 = true;
        if(llvm::StringRef(argv).contains("-std=c++20"))
            has_20 = true;
    }
    EXPECT_TRUE(has_17);
    EXPECT_TRUE(has_20);

    ASSERT_EQ(database.candidate_entries("other.cpp").size(), 1U);
};

TEST_CASE(CandidateOrderStable) {
    /// The default selection is content-decided: loading the same entries
    /// in a different order picks the same winner.
    CompilationDatabase first;
    first.add_command("fake", "main.cpp", "clang++ -std=c++17 main.cpp"sv);
    first.add_command("fake", "main.cpp", "clang++ -std=c++20 main.cpp"sv);

    CompilationDatabase second;
    second.add_command("fake", "main.cpp", "clang++ -std=c++20 main.cpp"sv);
    second.add_command("fake", "main.cpp", "clang++ -std=c++17 main.cpp"sv);

    EXPECT_EQ(first.selected_hash(first.paths().intern("main.cpp")),
              second.selected_hash(second.paths().intern("main.cpp")));
    EXPECT_EQ(print_argv(render_entry(first, "main.cpp")),
              print_argv(render_entry(second, "main.cpp")));
};

TEST_CASE(CodegenFilter) {
    /// Codegen-only options never reach the compile render.
    CompilationDatabase database;
    database.add_command(
        "fake",
        "main.cpp",
        "clang++ -std=c++20 -fPIC -fno-omit-frame-pointer -fstack-protector-strong "
        "-fdata-sections -ffunction-sections -flto -fcolor-diagnostics -g main.cpp"sv);

    auto argv = print_argv(render_entry(database, "main.cpp"));

    EXPECT_CONTAINS(argv, "-std=c++20");

    EXPECT_NOT_CONTAINS(argv, "-fPIC");
    EXPECT_NOT_CONTAINS(argv, "-fno-omit-frame-pointer");
    EXPECT_NOT_CONTAINS(argv, "-fstack-protector");
    EXPECT_NOT_CONTAINS(argv, "-fdata-sections");
    EXPECT_NOT_CONTAINS(argv, "-ffunction-sections");
    EXPECT_NOT_CONTAINS(argv, "-flto");
    EXPECT_NOT_CONTAINS(argv, "-fcolor-diagnostics");
    EXPECT_NOT_CONTAINS(argv, "-g");

    /// They survive in the full view.
    auto full =
        print_argv(database.render_full(database.candidate_entries("main.cpp").front().config));
    EXPECT_CONTAINS(full, "-fPIC");
    EXPECT_CONTAINS(full, "-flto");
};

TEST_CASE(DependencyScanFilter) {
    CompilationDatabase database;
    database.add_command("fake",
                         "main.cpp",
                         "clang++ -std=c++20 -MD -MF main.d -MT main.o main.cpp"sv);

    auto argv = print_argv(render_entry(database, "main.cpp"));

    EXPECT_CONTAINS(argv, "-std=c++20");
    EXPECT_NOT_CONTAINS(argv, "-MD");
    EXPECT_NOT_CONTAINS(argv, "-MF");
    EXPECT_NOT_CONTAINS(argv, "-MT");
    EXPECT_NOT_CONTAINS(argv, "main.d");
};

TEST_CASE(ModuleFilter) {
    /// A named module mapping is discarded (clice builds its own PCMs);
    /// the bare header-unit form stays part of the frontend semantics.
    EXPECT_STRIP("clang++ -std=c++20 -fmodule-file=m=mod.pcm main.cpp",
                 "clang++ -std=c++20 main.cpp");
    EXPECT_STRIP("clang++ -std=c++20 -fmodule-file=mod.pcm main.cpp",
                 "clang++ -std=c++20 -fmodule-file=mod.pcm main.cpp");
    EXPECT_STRIP("clang++ -std=c++20 -fprebuilt-module-path=/tmp main.cpp",
                 "clang++ -std=c++20 main.cpp");
};

TEST_CASE(UserContentClassification) {
    CompilationDatabase database;
    database.add_command("fake", "a.cpp", "clang++ -std=c++20 -Wall -DA=1 -DFOO a.cpp"sv);
    database.add_command("fake", "b.cpp", "clang++ -std=c++20 -Wall -DB=2 b.cpp"sv);

    auto a_argv = print_argv(render_entry(database, "a.cpp"));
    auto b_argv = print_argv(render_entry(database, "b.cpp"));

    EXPECT_CONTAINS(a_argv, "-std=c++20");
    EXPECT_CONTAINS(a_argv, "-Wall");
    EXPECT_CONTAINS(b_argv, "-std=c++20");
    EXPECT_CONTAINS(b_argv, "-Wall");

    EXPECT_CONTAINS(a_argv, "A=1");
    EXPECT_CONTAINS(a_argv, "FOO");
    EXPECT_CONTAINS(b_argv, "B=2");

    EXPECT_NOT_CONTAINS(a_argv, "B=2");
    EXPECT_NOT_CONTAINS(b_argv, "A=1");
};

TEST_CASE(IncludePathAbsolutize) {
    /// Relative include paths absolutize against the entry directory.
    CompilationDatabase database;
    database.add_command("/project/build",
                         "main.cpp",
                         "clang++ -Iinclude -isystem sys/inc -iquote ../src main.cpp"sv);

    auto result = render_entry(database, "main.cpp");

    /// Check each argument individually with separator normalization
    /// (print_argv escapes backslashes, breaking convert_to_slash on Windows).
    auto has_path = [](llvm::ArrayRef<const char*> args, llvm::StringRef needle) {
        for(auto* arg: args) {
            if(path::convert_to_slash(arg).find(needle.str()) != std::string::npos)
                return true;
        }
        return false;
    };

    EXPECT_TRUE(has_path(result, "/project/build/include"));
    EXPECT_TRUE(has_path(result, "/project/build/sys/inc"));
    EXPECT_TRUE(has_path(result, "/project/"));

    /// Absolute paths are kept as-is.
    CompilationDatabase database2;
    database2.add_command("/project/build", "main.cpp", "clang++ -I/usr/include main.cpp"sv);
    EXPECT_TRUE(has_path(render_entry(database2, "main.cpp"), "/usr/include"));
};

TEST_CASE(SemanticOptionsPreserved) {
    EXPECT_STRIP("clang++ -std=c++20 -fno-exceptions -fno-rtti -pedantic main.cpp",
                 "clang++ -std=c++20 -fno-exceptions -fno-rtti -pedantic main.cpp");
    EXPECT_STRIP("clang++ -std=c++20 -Wall -Werror main.cpp",
                 "clang++ -std=c++20 -Wall -Werror main.cpp");
};

TEST_CASE(ForcedLanguage) {
    CompilationDatabase database;
    database.add_command("fake", "a.h", "clang++ -x c++ a.h"sv);
    database.add_command("fake", "b.cpp", "clang++ b.cpp"sv);

    EXPECT_EQ(database.forced_language(database.candidate_entries("a.h").front().config), "c++");
    EXPECT_TRUE(
        database.forced_language(database.candidate_entries("b.cpp").front().config).empty());
};

/// Write JSON to a temp file, load into a CDB, remove the file.
/// Returns the number of entries loaded.
std::size_t load_json(CompilationDatabase& database, llvm::StringRef json) {
    auto path = fs::createTemporaryFile("cdb", "json");
    if(!path)
        return 0;
    {
        std::error_code ec;
        llvm::raw_fd_ostream out(*path, ec);
        if(ec)
            return 0;
        out << json;
    }
    auto count = database.load(*path).value_or(0);
    llvm::sys::fs::remove(*path);
    return count;
}

TEST_CASE(LoadMixedFormats) {
    /// "arguments" array and "command" string can coexist in the same CDB.
    TempDir tmp;
    auto dir = json_escape(tmp.root);
    CompilationDatabase database;
    auto count = load_json(database, R"([{"directory": ")" + dir + R"(", "file": "a.cpp",
          "arguments": ["clang++", "-std=c++20", "a.cpp"]},
         {"directory": ")" + dir + R"(", "file": "b.cpp",
          "command": "clang++ -std=c++23 b.cpp"}])");

    ASSERT_EQ(count, 2U);
    EXPECT_CONTAINS(print_argv(render_entry(database, tmp.path("a.cpp"))), "-std=c++20");
    EXPECT_CONTAINS(print_argv(render_entry(database, tmp.path("b.cpp"))), "-std=c++23");
};

TEST_CASE(RelativeDirectoryAnchored) {
    /// A relative CDB `directory` anchors to the CDB file's own location,
    /// both for resolving the entry's file and as the config's directory.
    TempDir tmp;
    CompilationDatabase database;
    tmp.touch("compile_commands.json", R"([
        {"directory": "build", "file": "main.cpp",
         "arguments": ["clang++", "-std=c++20", "main.cpp"]}
    ])");
    ASSERT_EQ(database.load(tmp.path("compile_commands.json")).value_or(0), 1U);

    auto file = path::join(tmp.root, "build", "main.cpp");
    auto candidates = database.candidate_entries(file);
    ASSERT_EQ(candidates.size(), 1U);
    EXPECT_EQ(llvm::StringRef(database.config(candidates.front().config).directory),
              path::join(tmp.root, "build"));
};

TEST_CASE(RelativeLoadPathAnchored) {
    /// A relative CDB path given to load() must not leak relative entry
    /// identities: the anchor base is absolutized first.
    TempDir tmp;
    tmp.touch("compile_commands.json", R"([
        {"directory": "build", "file": "main.cpp",
         "arguments": ["clang++", "-std=c++20", "main.cpp"]}
    ])");

    llvm::SmallString<256> saved_cwd;
    ASSERT_FALSE(bool(llvm::sys::fs::current_path(saved_cwd)));
    ASSERT_FALSE(bool(llvm::sys::fs::set_current_path(tmp.root)));
    auto restore = llvm::make_scope_exit([&] { llvm::sys::fs::set_current_path(saved_cwd); });

    CompilationDatabase database;
    ASSERT_EQ(database.load("compile_commands.json").value_or(0), 1U);

    auto candidates = database.candidate_entries(path::join(tmp.root, "build", "main.cpp"));
    ASSERT_EQ(candidates.size(), 1U);
    EXPECT_EQ(llvm::StringRef(database.config(candidates.front().config).directory),
              path::join(tmp.root, "build"));
};

TEST_CASE(LoadErrorRecovery) {
    /// Bad entries should be skipped; good entries still load.
    TempDir tmp;
    auto dir = json_escape(tmp.root);
    CompilationDatabase database;
    auto count = load_json(database,
                           R"([{"file": "no_dir.cpp",
          "arguments": ["clang++", "no_dir.cpp"]},
         {"directory": ")" + dir +
                               R"(",
          "arguments": ["clang++", "no_file.cpp"]},
         {"directory": ")" + dir +
                               R"(", "file": "no_args.cpp"},
         {"directory": ")" + dir +
                               R"(", "file": "good.cpp",
          "arguments": ["clang++", "-std=c++20", "good.cpp"]},
         42,
         {"directory": ")" + dir +
                               R"(", "file": "also_good.cpp",
          "command": "clang++ -Wall also_good.cpp"}])");

    ASSERT_EQ(count, 2U);
    EXPECT_CONTAINS(print_argv(render_entry(database, tmp.path("good.cpp"))), "-std=c++20");
    EXPECT_CONTAINS(print_argv(render_entry(database, tmp.path("also_good.cpp"))), "-Wall");
};

TEST_CASE(LoadCudaHeader) {
    /// .cuh entries are C-family despite clang's extension table; non-C
    /// entries some build systems emit are skipped.
    TempDir tmp;
    auto dir = json_escape(tmp.root);
    CompilationDatabase database;
    auto count = load_json(database, R"([{"directory": ")" + dir + R"(", "file": "kernels.cuh",
          "command": "nvcc -c kernels.cuh -o kernels.o"},
         {"directory": ")" + dir + R"(", "file": "app.rc",
          "command": "rc /fo app.res app.rc"}])");

    ASSERT_EQ(count, 1U);
    EXPECT_TRUE(database.has_entry(tmp.path("kernels.cuh")));
};

TEST_CASE(LoadEmptyCommand) {
    /// Whitespace-only or empty "command" should not crash.
    TempDir tmp;
    auto dir = json_escape(tmp.root);
    CompilationDatabase database;
    auto count = load_json(database,
                           R"([{"directory": ")" + dir + R"(", "file": "empty.cpp", "command": ""},
         {"directory": ")" + dir +
                               R"(", "file": "spaces.cpp", "command": "   "},
         {"directory": ")" + dir +
                               R"(", "file": "ok.cpp",
          "command": "clang++ -std=c++20 ok.cpp"}])");

    ASSERT_EQ(count, 1U);
    EXPECT_CONTAINS(print_argv(render_entry(database, tmp.path("ok.cpp"))), "-std=c++20");
};

TEST_CASE(LoadReload) {
    /// Second load() replaces all entries from the first.
    TempDir tmp;
    auto dir = json_escape(tmp.root);
    CompilationDatabase database;

    auto file_a = tmp.path("a.cpp");
    auto file_b = tmp.path("b.cpp");

    load_json(database, R"([{"directory": ")" + dir + R"(", "file": "a.cpp",
          "arguments": ["clang++", "-std=c++17", "a.cpp"]}])");
    EXPECT_CONTAINS(print_argv(render_entry(database, file_a)), "-std=c++17");

    auto count = load_json(database, R"([{"directory": ")" + dir + R"(", "file": "b.cpp",
          "arguments": ["clang++", "-std=c++23", "b.cpp"]}])");
    ASSERT_EQ(count, 1U);

    EXPECT_TRUE(database.candidate_entries(file_a).empty());
    EXPECT_CONTAINS(print_argv(render_entry(database, file_b)), "-std=c++23");
};

TEST_CASE(LoadCommandQuoting) {
    /// "command" string with spaces in paths and quoted defines.
    TempDir tmp;
    auto dir = json_escape(tmp.root);
    CompilationDatabase database;
    auto count = load_json(database, R"([{"directory": ")" + dir + R"(", "file": "main.cpp",
          "command": "clang++ -std=c++20 \"-DMSG=hello world\" -I\"/path with spaces\" main.cpp"}])");

    ASSERT_EQ(count, 1U);
    auto argv = print_argv(render_entry(database, tmp.path("main.cpp")));
    EXPECT_CONTAINS(argv, "hello world");
    EXPECT_CONTAINS(argv, "with spaces");
};

TEST_CASE(LoadRelativePath) {
    /// load() resolves relative file paths against the directory.
    TempDir tmp;
    auto project = tmp.path("project/build");
    auto other = tmp.path("other/build");
    CompilationDatabase database;
    auto count = load_json(database,
                           R"([{"directory": ")" + json_escape(project) +
                               R"(", "file": "src/main.cpp",
          "arguments": ["clang++", "-std=c++20", "src/main.cpp"]},
         {"directory": ")" + json_escape(other) +
                               R"(", "file": "src/main.cpp",
          "arguments": ["clang++", "-std=c++17", "src/main.cpp"]}])");

    ASSERT_EQ(count, 2U);

    EXPECT_CONTAINS(print_argv(render_entry(database, path::join(project, "src", "main.cpp"))),
                    "-std=c++20");
    EXPECT_CONTAINS(print_argv(render_entry(database, path::join(other, "src", "main.cpp"))),
                    "-std=c++17");

    /// A relative spelling is a different path — no entry.
    EXPECT_TRUE(database.candidate_entries("src/main.cpp").empty());
};

TEST_CASE(LoadDotSegments) {
    /// Entry paths intern without . and .. segments, so lookups against
    /// clang-reported (realpath'd) spellings match.
    TempDir tmp;
    auto build = tmp.path("project/build");
    CompilationDatabase database;
    auto count = load_json(database,
                           R"([{"directory": ")" + json_escape(build) +
                               R"(", "file": "../src/./main.cpp",
          "arguments": ["clang++", "-std=c++20", "../src/./main.cpp"]}])");

    ASSERT_EQ(count, 1U);
    EXPECT_TRUE(database.has_entry(tmp.path("project/src/main.cpp")));
};

TEST_CASE(ResourceDir) {
    CompilationDatabase database;
    database.add_command("/fake", "main.cpp", "clang++ -std=c++23 test.cpp"sv);

    auto& entry = database.candidate_entries("main.cpp").front();
    CommandRef ref{entry.file,
                   entry.config,
                   database.input_kind(entry.config, "main.cpp"),
                   CommandSource::CDBExact};
    auto argv = database.render_driver(ref);

    bool has_resource_dir = false;
    for(std::size_t i = 0; i + 1 < argv.size(); i += 1) {
        if(argv[i] == "-resource-dir"sv) {
            EXPECT_EQ(llvm::StringRef(argv[i + 1]), resource_dir());
            has_resource_dir = true;
            break;
        }
    }
    EXPECT_EQ(has_resource_dir, !resource_dir().empty());

    /// A command carrying its own resource dir is not double-injected.
    CompilationDatabase database2;
    database2.add_command("/fake", "main.cpp", "clang++ -resource-dir /custom main.cpp"sv);
    auto& entry2 = database2.candidate_entries("main.cpp").front();
    CommandRef ref2{entry2.file,
                    entry2.config,
                    database2.input_kind(entry2.config, "main.cpp"),
                    CommandSource::CDBExact};
    auto argv2 = database2.render_driver(ref2);
    int count = 0;
    for(auto* arg: argv2) {
        if(arg == "-resource-dir"sv) {
            count += 1;
        }
    }
    EXPECT_EQ(count, 1);
};

};  // TEST_SUITE(Command)

}  // namespace

}  // namespace clice::testing
