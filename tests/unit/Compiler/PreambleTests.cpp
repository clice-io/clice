#include "Test/Annotation.h"
#include "Test/Test.h"
#include "Compiler/Compilation.h"
#include "Compiler/Preamble.h"
#include "Compiler/Scan.h"

namespace clice::testing {

namespace {

TEST_SUITE(Preamble) {

void expect_bounds(std::vector<llvm::StringRef> marks, llvm::StringRef content) {
    auto annotation = AnnotatedSource::from(content);

    auto bounds = compute_preamble_bounds(annotation.content);

    ASSERT_EQ(bounds.size(), marks.size());

    for(std::uint32_t i = 0; i < bounds.size(); i++) {
        ASSERT_EQ(bounds[i], annotation.offsets[marks[i]]);
    }
}

void expect_build_pch(llvm::StringRef main_file,
                      llvm::StringRef test_contents,
                      llvm::StringRef preamble = "") {
    auto tmp = fs::createTemporaryFile("clice", "pch");
    ASSERT_TRUE(tmp.has_value());
    std::string output_path = std::move(*tmp);

    AnnotatedSources sources;
    sources.add_sources(test_contents);
    auto& files = sources.all_files;

    if(!preamble.empty()) {
        files.try_emplace("preamble.h", AnnotatedSource{.content = preamble.str()});
    }

    ASSERT_TRUE(files.contains(main_file));
    std::string content = files[main_file].content;
    files.erase(main_file);

    CompilationParams params;
    params.output_file = output_path;
    auto bound = compute_preamble_bound(content);
    params.add_remapped_file(main_file, content, bound);

    params.arguments = {
        "clang++",
        "-xc++",
        "-std=c++20",
    };

    if(!preamble.empty()) {
        params.arguments.emplace_back("--include=preamble.h");
    }

    std::string buffer = main_file.str();
    params.arguments.emplace_back(buffer.c_str());

    for(auto& [path, source]: files) {
        params.add_remapped_file(path::join(".", path), source.content);
    }

    /// Build PCH.
    PCHInfo info;
    {
        /// NOTE: PCH file is written when CompilerInstance is destructed.
        auto unit = compile(params, info);
        ASSERT_TRUE(unit.has_value());

        ASSERT_EQ(info.path, output_path);
        /// expect(that % info.command == params.arguments);
        /// TODO: expect(that % info.deps == deps);
    }

    /// Build AST with PCH.
    for(auto& [path, source]: files) {
        params.add_remapped_file(path::join(".", path), source.content);
    }

    params.add_remapped_file(main_file, content);
    params.pch = {info.path, info.preamble.size()};
    auto unit = compile(params);
    ASSERT_TRUE(unit.has_value());
};

TEST_CASE(Bounds) {
    expect_bounds({}, "int main(){}");

    expect_bounds({"0"}, "#include <iostream>$(0)");
    expect_bounds({"0"}, "#include <iostream>$(0)\n");

    expect_bounds({"0", "1", "2", "3"},
                  R"cpp(
#ifdef TEST$(0)
#include <iostream>$(1)
#define 1$(2)
#endif$(3)
)cpp");

    expect_bounds({"0"},
                  R"cpp(
#include <iostream>$(0)
int x = 1;
)cpp");

    expect_bounds({"0", "1"}, R"cpp(
module;$(0)
#include <iostream>$(1)
export module test;
)cpp");
};

TEST_CASE(TranslationUnit) {
    expect_build_pch("main.cpp",
                     R"cpp(
#[test.h]
int foo();

#[main.cpp]
#include "test.h"
int x = foo();
)cpp");
};

TEST_CASE(Module) {
    expect_build_pch("main.cpp",
                     R"cpp(
#[test.h]
int foo();

#[main.cpp]
module;
#include "test.h"
export module test;
export int x = foo();
)cpp");
};

TEST_CASE(Header) {
    llvm::StringRef test_contents = R"cpp(
#[test.h]
int bar();

#[test1.h]
#include "test.h"
Point x = {foo(), bar()};

#[test2.h]
struct Point {
    int x;
    int y;
};

#include "test1.h"

#[test3.h]
int foo();

#[main.cpp]
#include "test3.h"
#include "test2.h"
)cpp";

    AnnotatedSources sources;
    sources.add_sources(test_contents);
    auto& files = sources.all_files;
    ASSERT_TRUE(files.contains("main.cpp"));
    std::string content = files["main.cpp"].content;
    files.erase("main.cpp");

    std::string preamble;

    /// Compute implicit include.
    {
        CompilationParams params;
        params.add_remapped_file("main.cpp", content);
        params.arguments = {"clang++", "-std=c++20", "main.cpp"};

        for(auto& [path, source]: files) {
            params.add_remapped_file(path::join(".", path), source.content);
        }

        auto unit = preprocess(params);
        ASSERT_TRUE(unit.has_value());

        auto path = path::join(".", "test1.h");
        auto fid = unit->file_id(path);
        ASSERT_TRUE(fid.isValid());

        while(fid.isValid()) {
            auto location = unit->include_location(fid);
            auto [fid2, offset] = unit->decompose_location(location);
            auto content = unit->file_content(fid2).substr(0, offset);

            /// Remove incomplete include.
            content = content.substr(0, content.rfind("\n"));
            preamble += content;
            fid = fid2;
        }
    }

    expect_build_pch("test1.h", test_contents, preamble);
};

TEST_CASE(Chain) {
    llvm::StringRef test_contents = R"cpp(
#[test.h]
int bar();

#[test2.h]
int foo();

#[main.cpp]
#include "test.h"
#include "test2.h"
int x = bar();
int y = foo();
)cpp";

    AnnotatedSources sources;
    sources.add_sources(test_contents);
    auto& files = sources.all_files;
    ASSERT_TRUE(files.contains("main.cpp"));
    std::string content = files["main.cpp"].content;
    files.erase("main.cpp");

    auto bounds = compute_preamble_bounds(content);

    CompilationParams params;
    params.arguments = {"clang++", "-std=c++20", "main.cpp"};

    PCHInfo info;
    std::uint32_t last_bound = 0;
    for(auto bound: bounds) {
        auto tmp = fs::createTemporaryFile("clice", "pch");
        ASSERT_TRUE(tmp.has_value());
        std::string outPath = std::move(*tmp);

        params.add_remapped_file("main.cpp", content, bound);
        if(params.output_file.empty()) {
            params.pch = {params.output_file.str().str(), last_bound};
        }

        params.output_file = outPath;
        last_bound = bound;

        for(auto& [path, source]: files) {
            params.add_remapped_file(path::join(".", path), source.content);
        }

        {
            auto unit = compile(params, info);
            ASSERT_TRUE(unit.has_value());

            ASSERT_EQ(info.path, outPath);
            /// expect(that % info.command == params.arguments);
        }
    }

    /// Build AST with PCH.
    for(auto& [path, source]: files) {
        params.add_remapped_file(path::join(".", path), source.content);
    }

    params.add_remapped_file("main.cpp", content);
    params.pch = {info.path, last_bound};
    auto unit = compile(params);
    ASSERT_TRUE(unit.has_value());
};

TEST_CASE(Scan) {
    llvm::StringRef content = R"(
            #include <iostream>
            #include "test/file"
            export module A:
        )";

    auto result = scan(content);
    for(auto& tok: result.module_name) {
        std::println("{}", tok.text(content));
    }

    for(auto& tok: result.includes) {
        std::println("include: {}", tok.file);
    }
};

};  // TEST_SUITE(Preamble)

}  // namespace

}  // namespace clice::testing
