#include "Test/CTest.h"
#include "Feature/CodeCompletion.h"

namespace clice::testing {

namespace {

TEST(Feature, CodeCompletion) {
    llvm::StringRef code = R"cpp(
#include <iostream>
#include <string>
#include <vector>
#include <map>
///#include <>
int foo = 2;

int main() {
    foo = 2;
    std::vec$(pos)tor
}
)cpp";

    Annotation annotation = {code};
    CompilationParams params;
    params.content = annotation.source();
    params.srcPath = "main.cpp";
    params.command = "clang++ -std=c++20 main.cpp";
    params.completion = {"main.cpp", annotation.offset("pos")};

    config::CodeCompletionOption options = {};
    auto result = feature::codeCompletion(params, options);
    // for(auto& item: result) {
    //     println("kind {} label {}", item.label, refl::enum_name(item.kind));
    // }
}

}  // namespace

}  // namespace clice::testing

